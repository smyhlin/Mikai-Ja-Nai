#include "PlayerShell.hpp"

#include "PlayerPopups.hpp"

#include <QCursor>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QWindow>
#include <QPainterPath>
#include <QShowEvent>
#include <QStyle>
#include <QVBoxLayout>

#include "ShellSliders.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"

namespace mplayer {
namespace shell {

namespace {

QString stateName(PlayerShell::State state) {
    switch (state) {
        case PlayerShell::State::DefaultFit: return QStringLiteral("DEFAULT_FIT");
        case PlayerShell::State::FullscreenActive: return QStringLiteral("FULLSCREEN_ACTIVE");
        case PlayerShell::State::FullscreenAutoHidden:
            return QStringLiteral("FULLSCREEN_AUTO_HIDDEN");
    }
    return QStringLiteral("DEFAULT_FIT");
}

QString cursorName(PlayerShell::State state) {
    switch (state) {
        case PlayerShell::State::FullscreenAutoHidden: return QStringLiteral("none");
        case PlayerShell::State::DefaultFit:
        case PlayerShell::State::FullscreenActive: return QStringLiteral("default");
    }
    return QStringLiteral("default");
}

}  // namespace

PlayerShell::PlayerShell(QWidget* parent, bool frameless) : QWidget(parent) {
    setWindowTitle(QStringLiteral("Media Lightbox"));
    setMinimumSize(kMinWindowWidth, kMinWindowHeight);
    if (parent != nullptr) {
        resize(parent->size());
    } else {
        resize(1280, 760);
    }
    setAcceptDrops(true);
    setMouseTracking(true);
    setAutoFillBackground(false);

    frameless_ = frameless && parent == nullptr;
    if (frameless_) {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    } else {
        setWindowFlags(Qt::Widget);
    }

    quality_ = QStringLiteral("HD");
    origin_ = QStringLiteral("UDream");

    hideTimer_.setSingleShot(true);
    connect(&hideTimer_, &QTimer::timeout, this, &PlayerShell::enterAutoHidden);

    buildSurface();
    buildWindowControls();
    buildTitleBar();
    buildChevrons();
    buildDock();
    buildFooter();
    buildOverlays();
    buildPictureMenu();
    // The chrome has to sit above the surface, and it already does: the surface
    // is the first child built, and Qt stacks later siblings above earlier
    // ones. That is stated once, here, rather than re-asserted on every layout
    // pass - layoutChrome runs from applyState, which runs on every input
    // event, and each raise() is an explicit z-order mutation that invalidates
    // the covered region even when the order is already correct.
    //
    // Nothing lowers the surface and no chrome widget is created after this
    // point, so the order established here is the order that holds.
    for (QWidget* widget : {static_cast<QWidget*>(windowControls_), static_cast<QWidget*>(titleBar_),
                            static_cast<QWidget*>(btnPrev_), static_cast<QWidget*>(btnNext_),
                            static_cast<QWidget*>(dock_), static_cast<QWidget*>(footerLeft_),
                            static_cast<QWidget*>(footerRight_), static_cast<QWidget*>(hud_),
                            static_cast<QWidget*>(center_)}) {
        widget->raise();
    }

    applyState();
    layoutChrome();
}

PlayerShell::~PlayerShell() = default;

// ═════════════════════════════════════════════════════════════════════════════
//  BUILDERS
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::buildSurface() {
    surface_ = new VideoSurface(this);
    surface_->setObjectName(QStringLiteral("videoSurface"));
    surface_->setMouseTracking(true);
    surface_->setFocusPolicy(Qt::NoFocus);
    // Input over the picture belongs to the shell, not the surface: a click
    // toggles playback and a drag is not a click. The Python needed a
    // _ReposedEvent shim for this because the surface was a foreign native
    // window; here it is an ordinary child, so an event filter is enough.
    surface_->installEventFilter(this);
}

void PlayerShell::buildWindowControls() {
    windowControls_ = new Panel(blackSoft(), 8.0, w15(), this);
    auto* lay = new QHBoxLayout(windowControls_);
    lay->setContentsMargins(4, 4, 4, 4);
    lay->setSpacing(2);

    // "Original File" is an action, so it lives with the other controls rather
    // than floating over the middle of the picture. A PillButton keeps its own
    // glyph and label, so it reads as a chip at the head of the row.
    btnOriginal_ = new PillButton(windowControls_);
    btnOriginal_->setObjectName(QStringLiteral("originalFileButton"));
    btnOriginal_->setToolTip(QStringLiteral("open original file"));
    connect(btnOriginal_, &PillButton::clicked, this, &PlayerShell::openOriginalRequested);
    lay->addWidget(btnOriginal_);

    btnMin_ = new IconButton(IconPainter::minimize, QStringLiteral("window_minimize"), 28, 15,
                             windowControls_);
    connect(btnMin_, &QPushButton::clicked, this, &QWidget::showMinimized);
    lay->addWidget(btnMin_);

    btnClose_ = new IconButton(IconPainter::close, QStringLiteral("exit_viewer  (Esc)"), 28, 15,
                               windowControls_, QColor(237, 66, 69, 190));
    connect(btnClose_, &QPushButton::clicked, this, &PlayerShell::exitViewer);
    lay->addWidget(btnClose_);
}

void PlayerShell::buildTitleBar() {
    // A frameless window has no OS title bar, so a windowed player had no
    // way to be moved: every pixel belonged either to the picture or to a
    // control. This is the grab handle that a decorated window would give.
    titleBar_ = new Panel(blackSoft(), 0.0, w15(), this);
    titleBar_->setObjectName(QStringLiteral("playerTitleBar"));
    titleBar_->setCursor(Qt::OpenHandCursor);
    auto* lay = new QHBoxLayout(titleBar_);
    lay->setContentsMargins(12, 0, 6, 0);
    lay->setSpacing(8);

    auto* caption = new QLabel(QStringLiteral("MIKAI"), titleBar_);
    caption->setObjectName(QStringLiteral("playerTitleCaption"));
    caption->setStyleSheet(QStringLiteral("background: transparent; color: %1;").arg(w70().name()));
    lay->addWidget(caption);
    lay->addStretch(1);

    titleMin_ = new IconButton(IconPainter::minimize, QStringLiteral("window_minimize"), 28, 15,
                               titleBar_, w70());
    connect(titleMin_, &QPushButton::clicked, this, &QWidget::showMinimized);
    lay->addWidget(titleMin_);

    titleMax_ = new IconButton(IconPainter::restore, QStringLiteral("toggle_window_mode"), 28, 15,
                               titleBar_, w70());
    connect(titleMax_, &QPushButton::clicked, this, &PlayerShell::windowModeToggled);
    lay->addWidget(titleMax_);

    titleClose_ = new IconButton(IconPainter::close, QStringLiteral("exit_viewer  (Esc)"), 28, 15,
                                 titleBar_, w70());
    connect(titleClose_, &QPushButton::clicked, this, &PlayerShell::exitViewer);
    lay->addWidget(titleClose_);
}
void PlayerShell::buildChevrons() {
    btnPrev_ = new IconButton(IconPainter::chevronLeft, QStringLiteral("load_previous_media  (<-)"),
                              42, 18, this, QColor(0, 0, 0, 165));
    btnPrev_->setTint(w70());
    connect(btnPrev_, &QPushButton::clicked, this, &PlayerShell::prevRequested);

    btnNext_ = new IconButton(IconPainter::chevronRight, QStringLiteral("load_next_media  (->)"),
                              42, 18, this, QColor(0, 0, 0, 165));
    btnNext_->setTint(w70());
    connect(btnNext_, &QPushButton::clicked, this, &PlayerShell::nextRequested);
}

void PlayerShell::buildDock() {
    dock_ = new Panel(dock(), 16.0, w15(), this);
    auto* dockLayout = new QVBoxLayout(dock_);
    dockLayout->setContentsMargins(16, 8, 16, 8);
    dockLayout->setSpacing(3);

    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(4);

    auto* volumeHost = new QWidget(dock_);
    volumeHost->setFixedWidth(kDockSideWidth);
    volumeHost->setAttribute(Qt::WA_StyledBackground, false);
    volumeHost->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* volumeLayout = new QHBoxLayout(volumeHost);
    volumeLayout->setContentsMargins(0, 0, 0, 0);
    vol_ = new VolumeGroup(volumeHost);
    connect(vol_, &VolumeGroup::volumeChanged, this, &PlayerShell::onVolume);
    connect(vol_, &VolumeGroup::muteClicked, this, &PlayerShell::toggleMute);
    volumeLayout->addWidget(vol_);
    volumeLayout->addStretch();
    topRow->addWidget(volumeHost);

    topRow->addStretch();
    btnPlay_ = new IconButton(IconPainter::play, QStringLiteral("toggle_playback  (Space)"),
                              kDockPlaySize, kDockPlayGlyph, dock_);
    btnPlay_->setObjectName(QStringLiteral("playerPlayToggle"));
    connect(btnPlay_, &QPushButton::clicked, this, &PlayerShell::onPlayClicked);
    topRow->addWidget(btnPlay_);
    topRow->addStretch();

    auto* actionHost = new QWidget(dock_);
    actionHost->setFixedWidth(kDockSideWidth);
    actionHost->setAttribute(Qt::WA_StyledBackground, false);
    actionHost->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* actions = new QHBoxLayout(actionHost);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(4);
    actions->setAlignment(Qt::AlignRight);

    btnPip_ = new IconButton(IconPainter::pip, QStringLiteral("toggle_picture_in_picture"),
                             kDockActionSize, kDockActionGlyph, actionHost);
    connect(btnPip_, &QPushButton::clicked, this, &PlayerShell::pipRequested);
    actions->addWidget(btnPip_);

    btnWindowMode_ = new IconButton(IconPainter::contract, QStringLiteral("windowed_mode"),
                                    kDockActionSize, kDockActionGlyph, actionHost);
    connect(btnWindowMode_, &QPushButton::clicked, this, &PlayerShell::windowModeToggled);
    actions->addWidget(btnWindowMode_);

    btnAudio_ = new IconButton(IconPainter::microphone,
                               QStringLiteral("Озвучення — вибрати дубляж  (T)"),
                               kDockActionSize, kDockActionGlyph, actionHost);
    connect(btnAudio_, &QPushButton::clicked, this, &PlayerShell::audioRequested);
    btnAudio_->hide();
    actions->addWidget(btnAudio_);

    btnSubs_ = new IconButton(IconPainter::captions,
                              QStringLiteral("Субтитри — вибрати доріжку  (C)"),
                              kDockActionSize, kDockActionGlyph, actionHost);
    connect(btnSubs_, &QPushButton::clicked, this, &PlayerShell::subtitlesRequested);
    btnSubs_->hide();
    actions->addWidget(btnSubs_);

    btnEnhance_ = new IconButton(IconPainter::spark, QStringLiteral("open_enhancements"),
                                 kDockActionSize, kDockActionGlyph, actionHost);
    connect(btnEnhance_, &QPushButton::clicked, this, &PlayerShell::openEnhancements);
    actions->addWidget(btnEnhance_);

    btnSettings_ = new IconButton(IconPainter::gear, QStringLiteral("toggle_settings_popover"),
                                  kDockActionSize, kDockActionGlyph, actionHost);
    connect(btnSettings_, &QPushButton::clicked, this, &PlayerShell::openSettings);
    actions->addWidget(btnSettings_);
    topRow->addWidget(actionHost);
    dockLayout->addLayout(topRow);

    auto* timeline = new QHBoxLayout();
    timeline->setContentsMargins(0, 0, 0, 0);
    timeline->setSpacing(8);
    lblElapsed_ = new QLabel(QStringLiteral("00:00"), dock_);
    lblElapsed_->setFont(monoFont(9));
    lblElapsed_->setStyleSheet(
        QStringLiteral("background: transparent; color: rgba(255,255,255,230);"));
    lblElapsed_->setFixedWidth(38);
    timeline->addWidget(lblElapsed_);

    scrub_ = new ScrubBar(dock_);
    scrub_->setMinimumWidth(120);
    connect(scrub_, &RangeSlider::valueChanged, this, &PlayerShell::onScrub);
    timeline->addWidget(scrub_, 1);

    lblRemaining_ = new QLabel(QStringLiteral("-00:00"), dock_);
    lblRemaining_->setFont(monoFont(9));
    lblRemaining_->setStyleSheet(
        QStringLiteral("background: transparent; color: rgba(255,255,255,140);"));
    lblRemaining_->setFixedWidth(42);
    timeline->addWidget(lblRemaining_);
    dockLayout->addLayout(timeline);

    QWidget* overlayHost = parentWidget() != nullptr ? parentWidget() : this;
    flyout_ = new SettingsFlyout(overlayHost);
    connect(flyout_, &SettingsFlyout::speedSelected, this, &PlayerShell::onRate);
    connect(flyout_, &SettingsFlyout::qualitySelected, this, &PlayerShell::qualityRequested);

    enhancementsPopup_ = new MenuPopup(QStringLiteral("ПОКРАЩЕННЯ"), overlayHost);
    enhancementsPopup_->setObjectName(QStringLiteral("playerEnhancementsPopup"));
    connect(enhancementsPopup_, &MenuPopup::picked, this, [this](const QVariant& payload) {
        if (payload.toString() == QStringLiteral("anime4k")) {
            openAnime4K();
        }
    });

    anime4kPopup_ = new MenuPopup(QStringLiteral("ANIME4K"), overlayHost);
    anime4kPopup_->setObjectName(QStringLiteral("playerAnime4KPopup"));
    connect(anime4kPopup_, &MenuPopup::picked, this, [this](const QVariant& payload) {
        onAnime4KMode(static_cast<anime4k::Mode>(payload.toInt()));
    });
    btnEnhance_->setVisible(!anime4k::findShadersDir().isEmpty());
    refreshEnhancementMenus();

    registerOverlay(flyout_);
    registerOverlay(enhancementsPopup_);
    registerOverlay(anime4kPopup_);
}

void PlayerShell::buildFooter() {
    footerLeft_ = new Panel(pill(), 16.0, w15(), this);
    footerLeft_->setObjectName(QStringLiteral("playerInfoChip"));
    auto* info = new QHBoxLayout(footerLeft_);
    info->setContentsMargins(12, 5, 12, 5);
    info->setSpacing(8);

    lblEpisode_ = new QLabel(episodeLabel_.isEmpty() ? QStringLiteral("MIKAI") : episodeLabel_, footerLeft_);
    lblEpisode_->setObjectName(QStringLiteral("playerEpisodeLabel"));
    QFont appFont = monoFont(9);
    appFont.setBold(true);
    lblEpisode_->setFont(appFont);
    lblEpisode_->setStyleSheet(QStringLiteral("background: transparent; color: #ffffff;"));
    info->addWidget(lblEpisode_);

    auto* divider = new QLabel(QStringLiteral("│"), footerLeft_);
    divider->setStyleSheet(
        QStringLiteral("background: transparent; color: rgba(255,255,255,70);"));
    info->addWidget(divider);

    lblNetworkStats_ = new QLabel(QStringLiteral("0.00 Mbps × — FPS"), footerLeft_);
    lblNetworkStats_->setObjectName(QStringLiteral("playerNetworkStats"));
    lblNetworkStats_->setFont(monoFont(8));
    lblNetworkStats_->setStyleSheet(
        QStringLiteral("background: transparent; color: rgba(255,255,255,185);"));
    info->addWidget(lblNetworkStats_);

    lblEnhancement_ = new QLabel(footerLeft_);
    lblEnhancement_->setObjectName(QStringLiteral("playerEnhancementStatus"));
    lblEnhancement_->setFont(monoFont(8));
    lblEnhancement_->setStyleSheet(
        QStringLiteral("background: transparent; color: #79dcf2;"));
    lblEnhancement_->hide();
    info->addWidget(lblEnhancement_);
    footerLeft_->setFixedHeight(kInfoChipHeight);

    footerRight_ = new Panel(blackSoft(), 8.0, w15(), this);
    auto* h = new QHBoxLayout(footerRight_);
    h->setContentsMargins(4, 4, 4, 4);
    h->setSpacing(4);

    btnKeys_ = new IconButton(IconPainter::keyboard, QStringLiteral("keyboard_shortcuts  (?)"), 28,
                              15, footerRight_);
    connect(btnKeys_, &QPushButton::clicked, this, &PlayerShell::openKeys);
    h->addWidget(btnKeys_);

    btnMore_ = new IconButton(IconPainter::more, QStringLiteral("open_extended_menu"), 28, 16,
                              footerRight_);
    connect(btnMore_, &QPushButton::clicked, this, &PlayerShell::openMenu);
    h->addWidget(btnMore_);

    QWidget* overlayHost = parentWidget() != nullptr ? parentWidget() : this;
    keys_ = new KeyHints(overlayHost);
    morePopup_ = new MenuPopup(QStringLiteral("ДІЇ"), overlayHost);
    morePopup_->setObjectName(QStringLiteral("playerMorePopup"));
    registerOverlay(keys_);
    registerOverlay(morePopup_);
    QVector<MenuEntry> entries;
    const auto addEntry = [&entries](const QString& text, const QString& action) {
        MenuEntry entry;
        entry.text = text;
        entry.payload = action;
        entries.append(entry);
    };
    addEntry(QStringLiteral("Копіювати посилання"), QStringLiteral("copy"));
    addEntry(QStringLiteral("Відкрити у папці"), QStringLiteral("open"));
    addEntry(QStringLiteral("Вихід  (Esc)"), QStringLiteral("exit"));
    morePopup_->setEntries(entries);
    connect(morePopup_, &MenuPopup::picked, this, [this](const QVariant& payload) {
        morePopup_->close();
        const QString action = payload.toString();
        if (action == QStringLiteral("copy")) {
            toast_->flash(QStringLiteral("URL copied"));
        } else if (action == QStringLiteral("open")) {
            toast_->flash(path_.isEmpty() ? QStringLiteral("—") : path_);
        } else if (action == QStringLiteral("exit")) {
            exitViewer();
        }
    });
}

void PlayerShell::buildPictureMenu() {
    // Built once and reused: a menu rebuilt on every right-click allocates a
    // popover and its rows each time, and this one is opened often.
    QWidget* overlayHost = parentWidget() != nullptr ? parentWidget() : this;
    pictureMenu_ = new MenuPopup(QString(), overlayHost);
    pictureMenu_->setObjectName(QStringLiteral("playerPictureMenu"));
    pictureMenu_->hide();
    registerOverlay(pictureMenu_);
    QVector<MenuEntry> entries;
    const auto add = [&entries](const QString& text, const QString& action) {
        MenuEntry entry;
        entry.text = text;
        entry.payload = action;
        entries.append(entry);
    };
    add(QStringLiteral("Копіювати кадр"), QStringLiteral("frame"));
    add(QStringLiteral("Порівняти якість"), QStringLiteral("compare"));
    add(QStringLiteral("Копіювати посилання"), QStringLiteral("copy"));
    add(QStringLiteral("Вихід  (Esc)"), QStringLiteral("exit"));
    pictureMenu_->setEntries(entries);
    connect(pictureMenu_, &MenuPopup::picked, this, [this](const QVariant& payload) {
        pictureMenu_->close();
        const QString action = payload.toString();
        if (action == QStringLiteral("frame")) {
            emit frameRequested();
        } else if (action == QStringLiteral("compare")) {
            emit compareRequested();
        } else if (action == QStringLiteral("copy")) {
            emit copyLinkRequested();
        } else if (action == QStringLiteral("exit")) {
            exitViewer();
        }
    });
}

void PlayerShell::openPictureMenu(const QPoint& position) {
    if (pictureMenu_ == nullptr) {
        return;
    }
    pictureMenu_->popupNear(mapToGlobal(position));
}
void PlayerShell::buildOverlays() {
    hud_ = new StateHUD(this);
    // the manifest readout duplicates the dock's numbers and is never shown
    hud_->hide();
    toast_ = new Toast(this);
    center_ = new CenterGlyph(this);
    center_->setKind(CenterGlyph::Kind::None);
}

// ═════════════════════════════════════════════════════════════════════════════
//  STATE MACHINE
// ═════════════════════════════════════════════════════════════════════════════

PlayerShell::State PlayerShell::state() const {
    if (hudHidden_) return State::FullscreenAutoHidden;
    if (fullscreen_) return State::FullscreenActive;
    return State::DefaultFit;
}

QString PlayerShell::stateName() const { return ::mplayer::shell::stateName(state()); }

void PlayerShell::applyState() {
    const State st = state();
    const QString name = ::mplayer::shell::stateName(st);
    const bool hidden = hudHidden_;

    // Fullscreen does NOT suppress the chrome. The shell is hosted in a dialog
    // that is itself fullscreen, so gating on "not fullscreen" meant half the
    // UI could never appear. Idle auto-hide is what hides them.
    //
    // The painted min/max/close are a substitute for a missing OS frame, so
    // they exist only where there is no frame: over a full monitor, or in a
    // standalone frameless window. Inside the hosted decorated window the real
    // title bar owns those actions, and showing both gives the viewer two
    // close buttons.
    windowControls_->setVisible(!hidden && (fullscreen_ || frameless_));
    titleBar_->setVisible(!hidden && !fullscreen_ && frameless_);
    // The glyph states the action the press would take, so the dock never
    // offers a mode the player is already in.
    if (btnWindowMode_ != nullptr) {
        btnWindowMode_->setIcon(fullscreen_ ? IconPainter::contract : IconPainter::expand);
        btnWindowMode_->setToolTip(fullscreen_ ? QStringLiteral("windowed_mode")
                                               : QStringLiteral("fullscreen_mode"));
    }
    btnPrev_->setVisible(!hidden && hasPreviousEpisode_);
    btnNext_->setVisible(!hidden && hasNextEpisode_);
    dock_->setVisible(!hidden);
    footerLeft_->setVisible(!hidden);
    footerRight_->setVisible(!hidden);
    if (hidden) {
        dismissOverlay();
    }

    setStateCursor();


    // QOpenGLWidget ignores stylesheets, so corner rounding is a real mask.
    const int radius = (rounded_ && !fullscreen_ && frameless_) ? 12 : 0;
    if (radius > 0) {
        QPainterPath path;
        path.addRoundedRect(QRectF(surface_->geometry()), radius, radius);
        surface_->setMask(QRegion(path.toFillPolygon().toPolygon()));
        hasMask_ = true;
    } else if (hasMask_) {
        // Tracked rather than asked of the widget: QWidget::mask() hands back a
        // QRegion by value, and this branch runs on every input event.
        surface_->clearMask();
        hasMask_ = false;
    }

    if (name != lastState_) {
        lastState_ = name;
        emit stateChanged(name);
    }
    layoutChrome();
    update();
}

void PlayerShell::setStateCursor() {
    setCursor(hudHidden_ ? Qt::BlankCursor : Qt::ArrowCursor);
}

// ── fullscreen / autohide ──────────────────────────────────────────────

// The host owns the window, so these setters sync composition and emit
// nothing; windowModeToggled() is the only place an intent leaves the shell.
void PlayerShell::enterFullscreen() {
    fullscreen_ = true;
    hudHidden_ = false;
    applyState();
    hideTimer_.start(kAutohideMs);
}

void PlayerShell::leaveFullscreen() {
    fullscreen_ = false;
    hudHidden_ = false;
    applyState();
    hideTimer_.start(kAutohideMs);
}

void PlayerShell::windowModeToggled() {
    const bool desired = !fullscreen_;
    if (desired) {
        enterFullscreen();
    } else {
        leaveFullscreen();
    }
    emit fullscreenChanged(desired);
}

void PlayerShell::registerOverlay(QWidget* overlay) {
    if (overlay == nullptr || overlays_.contains(overlay)) {
        return;
    }
    overlays_.append(overlay);
}

bool PlayerShell::dismissOverlay() {
    bool dismissed = false;
    for (QWidget* overlay : overlays_) {
        if (overlay->isVisible()) {
            overlay->close();
            dismissed = true;
        }
    }
    return dismissed;
}

bool PlayerShell::hasVisibleOverlay() const {
    for (const QWidget* overlay : overlays_) {
        if (overlay->isVisible()) {
            return true;
        }
    }
    return false;
}

void PlayerShell::enterAutoHidden() {
    // Never yank the chrome away mid-gesture: an open overlay or a live drag
    // means the user is interacting even though no move crossed the shell.
    if (hasVisibleOverlay()) {
        hideTimer_.start(kAutohideMs);
        return;
    }
    if (scrub_->isDragging() || vol_->isDragging() || resizingLeft_ || resizingTop_ ||
        resizingRight_ || resizingBottom_) {
        hideTimer_.start(kAutohideMs);
        return;
    }
    hudHidden_ = true;
    applyState();
}

void PlayerShell::wake() {
    if (hudHidden_) {
        hudHidden_ = false;
        applyState();
    }
    hideTimer_.start(kAutohideMs);
}

// ═════════════════════════════════════════════════════════════════════════════
//  LAYOUT
// ═════════════════════════════════════════════════════════════════════════════

QSize PlayerShell::fitted() const {
    if (fullscreen_) {
        // Fullscreen is the whole monitor: the surface takes the window edge to
        // edge, no letterboxing, no design margin, no aspect bars.
        return {width(), height()};
    }
    const int pad = kFrameMargin;
    const int chromeHeight = 150 + titleBarOffset_;
    const int vw = std::max(160, width() - 2 * (pad + 24));
    const int vh = std::max(100, height() - chromeHeight);
    if (static_cast<double>(vw) / vh > aspect_) {
        return {static_cast<int>(vh * aspect_), vh};
    }
    return {vw, static_cast<int>(vw / aspect_)};
}

void PlayerShell::layoutChrome() {
    const int w = width();
    const int h = height();

    // ── canvas ────────────────────────────────────────────────────
    const QSize fit = fitted();
    const int sw = fit.width();
    const int sh = fit.height();
    const int sx = (w - sw) / 2;
    const int sy = titleBarOffset_ + (h - titleBarOffset_ - sh) / 2;
    // Guarded: a geometry write on a QOpenGLWidget schedules a repaint, and
    // layoutChrome runs from applyState, which runs on every input event. The
    // common case is a layout pass that computes the rectangle it already has.
    const QRect canvas(sx, sy, sw, sh);
    if (surface_->geometry() != canvas) {
        surface_->setGeometry(canvas);
    }
    // devicePixelRatioF walks widget -> window -> screen, and both uses below
    // want the same number.
    const qreal ratio = surface_->devicePixelRatioF();
    const int outputWidth = std::max(1, static_cast<int>(std::round(sw * ratio)));
    const int outputHeight = std::max(1, static_cast<int>(std::round(sh * ratio)));
    if (outputWidth_ != outputWidth || outputHeight_ != outputHeight) {
        outputWidth_ = outputWidth;
        outputHeight_ = outputHeight;
        updateInfoChip();
    }

    // ── window controls (top-right) ────────────────────────────────
    // sizeHint() forces a full QHBoxLayout measure over three children, and
    // the ternary used to ask for it twice.
    const int hintedWidth = windowControls_->sizeHint().width();
    const int wcWidth = hintedWidth > 0 ? hintedWidth : 96;
    const int wcX = std::clamp(static_cast<int>(w * 0.945) + 4, 0, std::max(0, w - wcWidth - 4));
    windowControls_->setGeometry(wcX, std::max(6, static_cast<int>(h * 0.008)), wcWidth, 36);

    // ── title bar (windowed mode) ──────────────────────────────────
    if (titleBar_ != nullptr) {
        const int barHeight = 36;
        titleBar_->setGeometry(0, 0, w, barHeight);
        // No painted bar means no reserved strip.
        titleBarOffset_ = (fullscreen_ || !frameless_) ? 0 : barHeight;
    }

    // ── flank chevrons ─────────────────────────────────────────────
    const int cy = static_cast<int>(h * 0.5) - 21;
    btnPrev_->move(static_cast<int>(w * 0.012), cy);
    btnNext_->move(w - static_cast<int>(w * 0.012) - 42, cy);

    // ── dock (bottom-centre) ───────────────────────────────────────
    const int dw = std::min(kDockMaxWidth, w - 2 * kDockHorizontalInset);
    dock_->setGeometry((w - dw) / 2, h - kDockBottomInset - kDockHeight, dw, kDockHeight);

    // ── status and secondary actions ───────────────────────────────
    footerLeft_->adjustSize();
    footerLeft_->move(12, 8);
    footerRight_->adjustSize();
    footerRight_->move(w - footerRight_->width() - 12, h - footerRight_->height() - 8);

    // ── overlays ───────────────────────────────────────────────────
    // The HUD is hidden by default and holds a 7-row label tree, so measuring
    // it on every layout pass is ~17 widgets of work for a widget nobody can
    // see. It is measured when it is shown instead, which is also the only
    // time its size can differ from what is already cached.
    if (!hud_->isHidden()) {
        hud_->adjustSize();
        hud_->move(w - hud_->width() - 14, static_cast<int>(h * 0.30));
    }
    center_->move((w - center_->width()) / 2, (h - center_->height()) / 2);
    if (toast_->isVisible()) {
        toast_->move((w - toast_->width()) / 2, static_cast<int>(h * 0.80));
    }
}

void PlayerShell::toggleHud() {
    showHud_ = !showHud_;
    hud_->setVisible(showHud_ && !hudHidden_);
    applyState();
}

// ═════════════════════════════════════════════════════════════════════════════
//  HOST → SHELL
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::setMedia(const QString& path, const QString& origin, const QString& quality) {
    path_ = path;
    hasMedia_ = !path.isEmpty();
    if (!origin.isEmpty()) origin_ = origin;
    if (!quality.isEmpty()) setQuality(quality);

    const QString name =
        path.isEmpty() ? QStringLiteral("no media loaded") : QFileInfo(path).fileName();
    setWindowTitle(QStringLiteral("%1 · Media Lightbox").arg(name));
    position_ = 0.0;
    networkBytesPerSecond_ = 0.0;
    streamFramesPerSecond_ = 0.0;
    updateInfoChip();
    layoutChrome();
}

void PlayerShell::setDuration(double seconds) {
    duration_ = std::max(0.0, seconds);
    scrub_->setDuration(duration_);
    updateTimeLabels();
}

void PlayerShell::setPosition(double seconds) {
    position_ = std::max(0.0, seconds);
    updateTimeLabels();
}

void PlayerShell::setPlaying(bool playing) {
    playing_ = playing;
    updatePlayIcon();
    center_->setKind(playing ? CenterGlyph::Kind::None : CenterGlyph::Kind::Play);
}

void PlayerShell::updatePlayIcon() {
    // The shell caches the pause state, but the engine owns the truth: the
    // first playingChanged lands before the cache is primed, and after a PiP
    // hand-back the cache and the core can disagree. One property read, and a
    // play control stuck on pause while the video runs is the one icon a
    // viewer trusts absolutely.
    const bool running = surface_ != nullptr && surface_->isPlaying();
    btnPlay_->setIcon(running ? IconPainter::pause : IconPainter::play);
}

void PlayerShell::setEpisodeStepping(bool hasPrevious, bool hasNext) {
    if (hasPreviousEpisode_ == hasPrevious && hasNextEpisode_ == hasNext) {
        return;
    }
    hasPreviousEpisode_ = hasPrevious;
    hasNextEpisode_ = hasNext;
    // An arrow with nothing to load is a dead control, so it goes away rather
    // than inviting a click that cannot do anything.
    btnPrev_->setVisible(hasPrevious && !hudHidden_);
    btnNext_->setVisible(hasNext && !hudHidden_);
    update();
}

void PlayerShell::setEpisodeLabel(const QString& label) {
    episodeLabel_ = label;
    if (lblEpisode_ != nullptr) {
        lblEpisode_->setText(label.isEmpty() ? QStringLiteral("MIKAI") : label);
        footerLeft_->adjustSize();
    }
}

void PlayerShell::setVolume(double volume, bool fromHost) {
    volume_ = std::clamp(volume, 0.0, 1.0);
    vol_->setVolume(volume_);
    if (volume_ > 0 && muted_) {
        muted_ = false;
        vol_->setMuted(false);
    }
    if (fromHost) {
        toast_->flash(QStringLiteral("Volume %1 %").arg(static_cast<int>(volume_ * 100)));
    }
}

void PlayerShell::setMuted(bool muted) {
    muted_ = muted;
    vol_->setMuted(muted);
}

void PlayerShell::setRate(double rate) {
    rate_ = std::clamp(rate, 0.25, 2.0);
    flyout_->setRate(rate_);
}

void PlayerShell::setQuality(const QString& quality) {
    quality_ = quality.isEmpty() ? QStringLiteral("HD") : quality;
    flyout_->setQuality(quality_);
}

void PlayerShell::setAudioAvailable(bool available) {
    if (btnAudio_ != nullptr) {
        btnAudio_->setVisible(available);
        dock_->adjustSize();
        layoutChrome();
    }
}

void PlayerShell::setSubtitlesAvailable(bool available) {
    if (btnSubs_ != nullptr) {
        btnSubs_->setVisible(available);
        dock_->adjustSize();
        layoutChrome();
    }
}

void PlayerShell::setAspectRatio(double ratio) {
    if (ratio > 0.05 && ratio < 20.0) {
        aspect_ = ratio;
        layoutChrome();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  INTERNAL CONTROLS
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::updateTimeLabels() {
    const QString elapsed = formatTime(position_);
    if (elapsed != lblElapsed_->text()) lblElapsed_->setText(elapsed);
    const QString remaining = formatSigned(std::max(0.0, duration_ - position_));
    if (remaining != lblRemaining_->text()) lblRemaining_->setText(remaining);
    if (duration_ > 0 && !scrub_->isDragging()) {
        // The bar is painted at pixel resolution, so a fraction finer than one
        // pixel moves the handle to where it already is. Snapping first keeps
        // 59 of every 60 emissions from repainting the whole timeline.
        const double pixels = std::max(1.0, static_cast<double>(scrub_->width()));
        scrub_->setValueSilent(std::round(position_ / duration_ * pixels) / pixels);
    }
    if (showHud_ && !hudHidden_) {
        hud_->updateData(stateName(),
                         {{QStringLiteral("cursor"), cursorName(state())},
                          {QStringLiteral("t"),
                           QStringLiteral("%1 / %2").arg(formatTime(position_),
                                                        formatTime(duration_))},
                          {QStringLiteral("rate"), QStringLiteral("%1x").arg(rate_)},
                          {QStringLiteral("volume"), QStringLiteral("%1%2")
                                                            .arg(static_cast<int>(volume_ * 100))
                                                            .arg(muted_ ? QStringLiteral("  muted")
                                                                        : QString())}});
        hud_->adjustSize();
        layoutChrome();
    }
}

void PlayerShell::onPlayClicked() {
    if (!hasMedia_) {
        toast_->flash(QStringLiteral("No media loaded — drop a file to begin"));
        return;
    }
    emit playPauseRequested();
}

void PlayerShell::onScrub(double fraction) {
    const double t = fraction * duration_;
    position_ = t;
    lblElapsed_->setText(formatTime(t));
    lblRemaining_->setText(formatSigned(std::max(0.0, duration_ - t)));
    emit seekRequested(t);
}

void PlayerShell::onVolume(double volume) {
    setVolume(volume);
    emit volumeChanged(volume_);
    toast_->flash(QStringLiteral("Volume %1 %").arg(static_cast<int>(volume_ * 100)));
}

void PlayerShell::toggleMute() {
    muted_ = !muted_;
    vol_->setMuted(muted_);
    emit muteChanged(muted_);
    toast_->flash(muted_ ? QStringLiteral("Muted")
                         : QStringLiteral("Volume %1 %").arg(static_cast<int>(volume_ * 100)));
}

void PlayerShell::onRate(double rate) {
    setRate(rate);
    emit rateChanged(rate_);
    toast_->flash(QStringLiteral("Speed %1x").arg(rate_, 0, 'g', 3));
}

void PlayerShell::setQualityOptions(const QVector<SettingsFlyout::QualityOption>& options) {
    qualityOptions_ = options;
}

void PlayerShell::setAnime4KMode(anime4k::Mode mode) {
    anime4kMode_ = mode;
    refreshEnhancementMenus();
    updateInfoChip();
}

void PlayerShell::setStreamStats(double bytesPerSecond, double framesPerSecond) {
    networkBytesPerSecond_ = std::max(0.0, bytesPerSecond);
    streamFramesPerSecond_ = std::max(0.0, framesPerSecond);
    updateInfoChip();
}

void PlayerShell::setSourceSize(int width, int height) {
    sourceWidth_ = std::max(0, width);
    sourceHeight_ = std::max(0, height);
    updateInfoChip();
}

void PlayerShell::onAnime4KMode(anime4k::Mode mode) {
    setAnime4KMode(mode);
    QString label = QStringLiteral("Вимкнено");
    for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
        if (info.mode == mode) {
            label = info.label;
            break;
        }
    }
    toast_->flash(QStringLiteral("Anime4K: %1").arg(label));
    emit anime4KModeChanged(mode);
}

void PlayerShell::refreshEnhancementMenus() {
    QVector<MenuEntry> enhancements;
    MenuEntry anime4kEntry;
    anime4kEntry.text = QStringLiteral("Anime4K  ›");
    anime4kEntry.payload = QStringLiteral("anime4k");
    anime4kEntry.current = anime4kMode_ != anime4k::Mode::Off;
    enhancements.append(anime4kEntry);
    enhancementsPopup_->setEntries(enhancements);
    anime4kRow_ = enhancementsPopup_->rowWidget(0);
    if (anime4kRow_ != nullptr) {
        anime4kRow_->installEventFilter(this);
    }

    QVector<MenuEntry> modes;
    for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
        MenuEntry entry;
        entry.text = info.label;
        entry.tooltip = info.hint;
        entry.payload = static_cast<int>(info.mode);
        entry.current = info.mode == anime4kMode_;
        modes.append(entry);
    }
    anime4kPopup_->setEntries(modes);
}

void PlayerShell::updateInfoChip() {
    const QString throughput =
        QStringLiteral("%1 Mbps").arg(networkBytesPerSecond_ * 8.0 / 1'000'000.0, 0, 'f', 2);
    const QString fps =
        streamFramesPerSecond_ > 0.0
            ? QStringLiteral("%1 FPS").arg(streamFramesPerSecond_, 0, 'f', 2)
            : QStringLiteral("— FPS");
    lblNetworkStats_->setText(QStringLiteral("%1 × %2").arg(throughput, fps));

    if (anime4kMode_ == anime4k::Mode::Off) {
        lblEnhancement_->hide();
    } else {
        QString label = QStringLiteral("Anime4K");
        for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
            if (info.mode == anime4kMode_) {
                label = info.label;
                break;
            }
        }
        const int sourceWidth = sourceWidth_ > 0 ? sourceWidth_ : outputWidth_;
        const int sourceHeight = sourceHeight_ > 0 ? sourceHeight_ : outputHeight_;
        lblEnhancement_->setText(
            QStringLiteral("│ Anime4K %1 · %2×%3 → %4×%5")
                .arg(label)
                .arg(sourceWidth)
                .arg(sourceHeight)
                .arg(outputWidth_)
                .arg(outputHeight_));
        lblEnhancement_->show();
    }
    footerLeft_->adjustSize();
}

void PlayerShell::openEnhancements() {
    dismissOverlay();
    wake();
    refreshEnhancementMenus();
    enhancementsPopup_->popupNear(btnEnhance_->mapToGlobal(QPoint(btnEnhance_->width(), 0)));
}

void PlayerShell::openAnime4K() {
    // The enhancements popover stays open: this is a submenu reached from it,
    // and dismissOverlay() would close the row the pointer is resting on.
    if (flyout_->isVisible()) {
        flyout_->close();
    }
    if (anime4kPopup_->isVisible()) {
        anime4kPopup_->close();
    }
    wake();
    refreshEnhancementMenus();
    anime4kPopup_->popupNear(btnEnhance_->mapToGlobal(QPoint(btnEnhance_->width(), 0)));
}

void PlayerShell::openSettings() {
    dismissOverlay();
    wake();
    flyout_->setRate(rate_);
    flyout_->setQuality(quality_);
    flyout_->setQualityOptions(qualityOptions_);
    flyout_->popupNear(btnSettings_->mapToGlobal(QPoint(btnSettings_->width(), 0)));
}

void PlayerShell::openKeys() {
    dismissOverlay();
    wake();
    keys_->popupNear(btnKeys_->mapToGlobal(QPoint(0, 0)));
}

void PlayerShell::openMenu() {
    dismissOverlay();
    wake();
    morePopup_->popupNear(btnMore_->mapToGlobal(QPoint(btnMore_->width(), 0)));
}

void PlayerShell::exitViewer() {
    emit closeRequested();
    close();
}

// ═════════════════════════════════════════════════════════════════════════════
//  KEYBOARD
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::keyPressEvent(QKeyEvent* event) {
    wake();
    switch (event->key()) {
        case Qt::Key_Space:
            onPlayClicked();
            break;
        case Qt::Key_F:
        case Qt::Key_F11:
            windowModeToggled();
            break;
        case Qt::Key_M:
            toggleMute();
            break;
        case Qt::Key_Escape:
            if (dismissOverlay()) {
                break;
            }
            if (fullscreen_) {
                windowModeToggled();
            } else {
                exitViewer();
            }
            break;
        case Qt::Key_Right:
            if (duration_ > 0) {
                const double t = std::min(duration_, position_ + 5);
                position_ = t;
                emit seekRequested(t);
                updateTimeLabels();
            } else {
                emit nextRequested();
            }
            break;
        case Qt::Key_Left:
            if (duration_ > 0) {
                const double t = std::max(0.0, position_ - 5);
                position_ = t;
                emit seekRequested(t);
                updateTimeLabels();
            } else {
                emit prevRequested();
            }
            break;
        case Qt::Key_Up:
        case Qt::Key_Down: {
            const double v = std::clamp(volume_ + (event->key() == Qt::Key_Up ? 0.1 : -0.1), 0.0, 1.0);
            setVolume(v);
            emit volumeChanged(v);
            toast_->flash(QStringLiteral("Volume %1 %").arg(static_cast<int>(v * 100)));
            break;
        }
        case Qt::Key_D:
            toggleHud();
            break;
        case Qt::Key_Question:
        case Qt::Key_Slash:
            openKeys();
            break;
        default:
            QWidget::keyPressEvent(event);
            return;
    }
    event->accept();
}

// ═════════════════════════════════════════════════════════════════════════════
//  POINTER
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    windowModeToggled();
    event->accept();
}

bool PlayerShell::hitEdges(const QPointF& pos) {
    if (!frameless_ || fullscreen_) return false;
    const int grip = kEdgeGrip;
    resizingLeft_ = pos.x() < grip;
    resizingTop_ = pos.y() < grip;
    resizingRight_ = pos.x() > width() - grip;
    resizingBottom_ = pos.y() > height() - grip;
    return resizingLeft_ || resizingTop_ || resizingRight_ || resizingBottom_;
}

void PlayerShell::mousePressEvent(QMouseEvent* event) {
    wake();
    if (event->button() == Qt::RightButton) {
        // The right button is the pointer, not a control: a click on the dock
        // or a popover is handled by those widgets, and this only sees what
        // reaches the picture.
        openPictureMenu(event->position().toPoint());
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    if (dismissOverlay()) {
        event->accept();
        return;
    }

    // The bar is the window's grab handle. startSystemMove hands the drag to
    // the window manager, which is what makes the player snap to edges and
    // land on another monitor the way any other window does; a hand-rolled
    // move() fights all of that.
    if (titleBar_ != nullptr && titleBar_->isVisible() && !fullscreen_
        && titleBar_->geometry().contains(static_cast<int>(event->position().x()),
                                          static_cast<int>(event->position().y()))) {
        if (QWindow* handle = windowHandle(); handle != nullptr) {
            handle->startSystemMove();
            event->accept();
            return;
        }
    }

    if (hitEdges(event->position())) {
        dragGeometry_ = geometry();
        dragOrigin_ = event->globalPosition().toPoint();
        grabPointer();
        event->accept();
        return;
    }

    press_ = event->position();
    moved_ = false;
    grabPointer();
    event->accept();
}

void PlayerShell::mouseMoveEvent(QMouseEvent* event) {
    wake();

    if (resizingLeft_ || resizingTop_ || resizingRight_ || resizingBottom_) {
        QRect g = dragGeometry_;
        const QPoint delta = event->globalPosition().toPoint() - dragOrigin_;
        if (resizingRight_) g.setWidth(std::max(kMinWindowWidth, g.width() + delta.x()));
        if (resizingBottom_) g.setHeight(std::max(kMinWindowHeight, g.height() + delta.y()));
        if (resizingLeft_) {
            g.setLeft(std::min(dragGeometry_.right() - kMinWindowWidth, dragGeometry_.x() + delta.x()));
        }
        if (resizingTop_) {
            g.setTop(std::min(dragGeometry_.bottom() - kMinWindowHeight, dragGeometry_.y() + delta.y()));
        }
        setGeometry(g);
        event->accept();
        return;
    }

    if (!(event->buttons() & Qt::LeftButton)) {
        // Edge cursor feedback; it must not fight the state cursor.
        if (!hudHidden_ && hitEdges(event->position())) {
            if ((resizingLeft_ && resizingTop_) || (resizingRight_ && resizingBottom_)) {
                setCursor(Qt::SizeFDiagCursor);
            } else if ((resizingLeft_ && resizingBottom_) || (resizingRight_ && resizingTop_)) {
                setCursor(Qt::SizeBDiagCursor);
            } else if (resizingLeft_ || resizingRight_) {
                setCursor(Qt::SizeHorCursor);
            } else {
                setCursor(Qt::SizeVerCursor);
            }
            event->accept();
            return;
        }
        if (hudHidden_ == false) setStateCursor();
        return;
    }

    const double dx = event->position().x() - press_.x();
    const double dy = event->position().y() - press_.y();
    if (!moved_ && std::hypot(dx, dy) < kDragThresholdPx) return;
    moved_ = true;
    event->accept();
}

void PlayerShell::mouseReleaseEvent(QMouseEvent* event) {
    if (!grabbed_) return;
    releasePointer();
    if (resizingLeft_ || resizingTop_ || resizingRight_ || resizingBottom_) {
        resizingLeft_ = resizingTop_ = resizingRight_ = resizingBottom_ = false;
        setStateCursor();
        event->accept();
        return;
    }
    setStateCursor();
    if (!moved_ && event->button() == Qt::LeftButton) onPlayClicked();
    event->accept();
}

void PlayerShell::grabPointer() {
    if (!grabbed_) {
        grabbed_ = true;
        grabMouse();
    }
}

void PlayerShell::releasePointer() {
    if (grabbed_) {
        grabbed_ = false;
        releaseMouse();
    }
}

bool PlayerShell::eventFilter(QObject* watched, QEvent* event) {
    // Hover opens the Anime4K submenu from inside the open enhancements popover,
    // but NOT the popover itself - that one is click-only. A menu that appears
    // the instant the pointer arrives makes it impossible to approach on
    // purpose; a submenu reached by resting on its one row is the ordinary
    // submenu gesture.
    if (watched == anime4kRow_ && event->type() == QEvent::Enter) {
        if (anime4kPopup_ != nullptr && !anime4kPopup_->isVisible()) {
            openAnime4K();
        }
        return false;
    }
    if (watched == surface_) {
        // The surface is a real child widget, so re-targeting is a matter of
        // translating the position into shell coordinates. The Python needed a
        // whole shim class because its surface was a foreign native window.
        switch (event->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonDblClick:
            case QEvent::MouseMove:
            case QEvent::MouseButtonRelease:
            case QEvent::Wheel: {
                QMouseEvent* mouse = static_cast<QMouseEvent*>(event);
                // A QOpenGLWidget emits a synthetic mouse move on every repaint.
                // Forwarding those would restart the auto-hide countdown on
                // every decoded frame and the chrome would never disappear.
                if (event->type() == QEvent::MouseMove) {
                    const QPoint at = mouse->globalPosition().toPoint();
                    if (at == lastMousePos_) return true;
                    lastMousePos_ = at;
                }
                if (event->type() == QEvent::MouseButtonPress) {
                    mousePressEvent(mouse);
                } else if (event->type() == QEvent::MouseButtonDblClick) {
                    mouseDoubleClickEvent(mouse);
                } else if (event->type() == QEvent::MouseMove) {
                    mouseMoveEvent(mouse);
                } else if (event->type() == QEvent::MouseButtonRelease) {
                    mouseReleaseEvent(mouse);
                }
                return true;
            }
            default:
                break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ═════════════════════════════════════════════════════════════════════════════
//  DRAG & DROP
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void PlayerShell::dropEvent(QDropEvent* event) {
    const QList<QUrl> paths = event->mimeData()->urls();
    for (const QUrl& url : paths) {
        const QString candidate = url.toLocalFile();
        const QString lower = candidate.toLower();
        static const QStringList kVideoSuffixes = {
            QStringLiteral(".mp4"), QStringLiteral(".mkv"), QStringLiteral(".webm"),
            QStringLiteral(".avi"), QStringLiteral(".mov"), QStringLiteral(".m4v"),
            QStringLiteral(".ts"),  QStringLiteral(".flv")};
        for (const QString& suffix : kVideoSuffixes) {
            if (lower.endsWith(suffix)) {
                emit mediaDropped(candidate);
                event->acceptProposedAction();
                return;
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  PAINT & LIFECYCLE
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    if (fullscreen_) {
        p.fillRect(rect(), bg0());
    } else {
        QRadialGradient g(QPointF(width() / 2.0, height() * 0.40),
                          std::max(width(), height()) * 0.75);
        g.setColorAt(0.0, QColor(5, 6, 10, 165));
        g.setColorAt(1.0, QColor(5, 6, 10, 250));
        p.fillRect(rect(), g);
    }

    // Chevron discs sit under their buttons.
    if (btnPrev_->isVisible()) {
        for (IconButton* button : {btnPrev_, btnNext_}) {
            paintRounded(p, QRectF(button->geometry()), 21, QColor(0, 0, 0, 110), w15());
        }
    }

    // Empty-state hint.
    if (!hasMedia_ && !hudHidden_) {
        const QFont f = uiFont(11);
        const QStringList lines = {QStringLiteral("no media loaded"),
                                   QStringLiteral("drop a video file here  ·  or pass one as argv[1]")};
        p.setFont(f);
        for (int i = 0; i < lines.size(); ++i) {
            p.setPen(i == 0 ? w55() : w35());
            p.drawText(QRectF(0, height() / 2.0 - 30 + i * 22, width(), 22), Qt::AlignHCenter,
                       lines[i]);
        }
    }
}

void PlayerShell::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layoutChrome();
}

void PlayerShell::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    applyState();
    layoutChrome();
    hideTimer_.start(kAutohideMs);
}

void PlayerShell::closeEvent(QCloseEvent* event) {
    releasePointer();
    resizingLeft_ = resizingTop_ = resizingRight_ = resizingBottom_ = false;
    hideTimer_.stop();
    dismissOverlay();
    QWidget::closeEvent(event);
}

}  // namespace shell
}  // namespace mplayer
