#include "PlayerShell.hpp"

#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
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
        case PlayerShell::State::ScrollOut: return QStringLiteral("SCROLL_OUT");
        case PlayerShell::State::ZoomInPan: return QStringLiteral("ZOOM_IN_PAN");
        case PlayerShell::State::FullscreenActive: return QStringLiteral("FULLSCREEN_ACTIVE");
        case PlayerShell::State::FullscreenAutoHidden:
            return QStringLiteral("FULLSCREEN_AUTO_HIDDEN");
    }
    return QStringLiteral("DEFAULT_FIT");
}

QString cursorName(PlayerShell::State state, bool panning) {
    switch (state) {
        case PlayerShell::State::FullscreenAutoHidden: return QStringLiteral("none");
        case PlayerShell::State::ZoomInPan: return panning ? QStringLiteral("grabbing")
                                                          : QStringLiteral("grab");
        case PlayerShell::State::ScrollOut: return QStringLiteral("zoom-in");
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
    buildChevrons();
    buildDock();
    buildFooter();
    buildOverlays();

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
    // toggles playback, a drag pans, the wheel zooms. The Python needed a
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

    btnWin_ = new IconButton(IconPainter::restore, QStringLiteral("toggle_window_mode"), 28, 15,
                             windowControls_);
    connect(btnWin_, &QPushButton::clicked, this, [this] {
        rounded_ = !rounded_;
        applyState();
    });
    lay->addWidget(btnWin_);

    btnClose_ = new IconButton(IconPainter::close, QStringLiteral("exit_viewer  (Esc)"), 28, 15,
                               windowControls_, QColor(237, 66, 69, 190));
    connect(btnClose_, &QPushButton::clicked, this, &PlayerShell::exitViewer);
    lay->addWidget(btnClose_);
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
    dock_ = new Panel(dock(), 9.0, w15(), this);
    auto* lay = new QHBoxLayout(dock_);
    lay->setContentsMargins(11, 7, 11, 7);
    lay->setSpacing(8);

    vol_ = new VolumeGroup(dock_);
    connect(vol_, &VolumeGroup::volumeChanged, this, &PlayerShell::onVolume);
    connect(vol_, &VolumeGroup::muteClicked, this, &PlayerShell::toggleMute);
    lay->addWidget(vol_);

    btnPlay_ = new IconButton(IconPainter::play, QStringLiteral("toggle_playback  (Space)"), 28, 15,
                              dock_);
    connect(btnPlay_, &QPushButton::clicked, this, &PlayerShell::onPlayClicked);
    lay->addWidget(btnPlay_);

    lblElapsed_ = new QLabel(QStringLiteral("00:00"), dock_);
    lblElapsed_->setFont(monoFont(9));
    lblElapsed_->setStyleSheet(QStringLiteral("background: transparent; color: rgba(255,255,255,230);"));
    lblElapsed_->setFixedWidth(34);
    lay->addWidget(lblElapsed_);

    scrub_ = new ScrubBar(dock_);
    scrub_->setMinimumWidth(120);
    connect(scrub_, &RangeSlider::valueChanged, this, &PlayerShell::onScrub);
    lay->addWidget(scrub_, 1);

    lblRemaining_ = new QLabel(QStringLiteral("-00:00"), dock_);
    lblRemaining_->setFont(monoFont(9));
    lblRemaining_->setStyleSheet(QStringLiteral("background: transparent; color: rgba(255,255,255,140);"));
    lblRemaining_->setFixedWidth(38);
    lay->addWidget(lblRemaining_);

    lay->addSpacing(2);

    btnFullscreen_ = new IconButton(IconPainter::expand,
                                     QStringLiteral("toggle_fullscreen_mode  (F)"), 28, 15, dock_);
    connect(btnFullscreen_, &QPushButton::clicked, this, [this] {
        if (fullscreen_) {
            exitFullscreen();
        } else {
            enterFullscreen();
        }
    });
    lay->addWidget(btnFullscreen_);

    btnPip_ = new IconButton(IconPainter::pip, QStringLiteral("toggle_picture_in_picture"), 28, 15,
                             dock_);
    connect(btnPip_, &QPushButton::clicked, this, &PlayerShell::pipRequested);
    lay->addWidget(btnPip_);

    btnAudio_ = new IconButton(IconPainter::microphone,
                               QStringLiteral("Озвучення — вибрати дубляж  (T)"), 28, 15, dock_);
    connect(btnAudio_, &QPushButton::clicked, this, &PlayerShell::audioRequested);
    btnAudio_->hide();
    lay->addWidget(btnAudio_);

    btnSubs_ = new IconButton(IconPainter::captions,
                              QStringLiteral("Субтитри — вибрати доріжку  (C)"), 28, 15, dock_);
    connect(btnSubs_, &QPushButton::clicked, this, &PlayerShell::subtitlesRequested);
    btnSubs_->hide();
    lay->addWidget(btnSubs_);

    btnSettings_ = new IconButton(IconPainter::gear, QStringLiteral("toggle_settings_popover"), 28,
                                  15, dock_);
    connect(btnSettings_, &QPushButton::clicked, this, &PlayerShell::openSettings);
    lay->addWidget(btnSettings_);

    flyout_ = new SettingsFlyout(this);
    connect(flyout_, &SettingsFlyout::speedSelected, this, &PlayerShell::onRate);
    connect(flyout_, &SettingsFlyout::qualitySelected, this, &PlayerShell::qualityRequested);

    dock_->adjustSize();
}

void PlayerShell::buildFooter() {
    footerLeft_ = new QWidget(this);
    footerLeft_->setAttribute(Qt::WA_TranslucentBackground);
    auto* v = new QVBoxLayout(footerLeft_);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(1);
    lblFile_ = new QLabel(QStringLiteral("no media loaded"), footerLeft_);
    lblFile_->setFont(monoFont(8));
    lblFile_->setStyleSheet(QStringLiteral("background: transparent; color: rgba(255,255,255,130);"));
    v->addWidget(lblFile_);
    lblOrigin_ = new QLabel(QStringLiteral("drop a file · or pass one as argv[1]"), footerLeft_);
    lblOrigin_->setFont(uiFont(9));
    lblOrigin_->setStyleSheet(QStringLiteral("background: transparent; color: rgba(255,255,255,85);"));
    v->addWidget(lblOrigin_);

    footerRight_ = new Panel(blackSoft(), 8.0, w15(), this);
    auto* h = new QHBoxLayout(footerRight_);
    h->setContentsMargins(4, 4, 4, 4);
    h->setSpacing(4);

    btnReset_ = new IconButton(IconPainter::reset, QStringLiteral("reset_canvas_transform  (R / 0)"),
                               28, 14, footerRight_);
    connect(btnReset_, &QPushButton::clicked, this, &PlayerShell::resetTransform);
    h->addWidget(btnReset_);

    btnKeys_ = new IconButton(IconPainter::keyboard, QStringLiteral("keyboard_shortcuts  (?)"), 28,
                              15, footerRight_);
    connect(btnKeys_, &QPushButton::clicked, this, &PlayerShell::openKeys);
    h->addWidget(btnKeys_);

    btnMore_ = new IconButton(IconPainter::more, QStringLiteral("open_extended_menu"), 28, 16,
                              footerRight_);
    connect(btnMore_, &QPushButton::clicked, this, &PlayerShell::openMenu);
    h->addWidget(btnMore_);

    keys_ = new KeyHints(this);
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
    if (scale_ > 1.005) return State::ZoomInPan;
    if (scale_ < 0.995) return State::ScrollOut;
    return State::DefaultFit;
}

QString PlayerShell::stateName() const { return ::mplayer::shell::stateName(state()); }

void PlayerShell::applyState() {
    const State st = state();
    const QString name = ::mplayer::shell::stateName(st);
    const bool hidden = hudHidden_;
    const bool zoom = scale_ > 1.005;

    // Fullscreen does NOT suppress the chrome. The shell is hosted in a dialog
    // that is itself fullscreen, so gating on "not fullscreen" meant half the
    // UI could never appear. Idle auto-hide is what hides them.
    windowControls_->setVisible(!hidden);
    btnPrev_->setVisible(!hidden && !zoom);
    btnNext_->setVisible(!hidden && !zoom);
    dock_->setVisible(!hidden);
    footerLeft_->setVisible(!hidden);
    footerRight_->setVisible(!hidden);
    if (hidden) {
        flyout_->close();
        keys_->close();
    }

    setStateCursor();

    // Rounded frame in windowed mode, hard edge in fullscreen. QOpenGLWidget does
    // not honour a stylesheet, so the corner rounding is a real mask.
    const int radius = (rounded_ && !fullscreen_) ? 12 : 0;
    if (radius > 0) {
        QPainterPath path;
        path.addRoundedRect(QRectF(surface_->geometry()), radius, radius);
        surface_->setMask(QRegion(path.toFillPolygon().toPolygon()));
    } else if (!surface_->mask().isEmpty()) {
        surface_->clearMask();
    }

    if (name != lastState_) {
        lastState_ = name;
        emit stateChanged(name);
    }
    layoutChrome();
    update();
}

void PlayerShell::setStateCursor() {
    if (hudHidden_) {
        setCursor(Qt::BlankCursor);
    } else if (scale_ > 1.005) {
        setCursor(panning_ ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
    } else if (scale_ < 0.995) {
        setCursor(Qt::PointingHandCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

// ── fullscreen / autohide ──────────────────────────────────────────────

void PlayerShell::enterFullscreen() {
    fullscreen_ = true;
    hudHidden_ = false;
    btnFullscreen_->setIcon(IconPainter::contract);
    applyState();
    hideTimer_.start(kAutohideMs);
    emit fullscreenChanged(true);
}

void PlayerShell::exitFullscreen() {
    fullscreen_ = false;
    hudHidden_ = false;
    // The host dialog owns the window geometry, so leaving fullscreen only
    // restores the centred preview - and the preview hides its chrome on idle
    // too, so the countdown is rearmed rather than stopped.
    hideTimer_.start(kAutohideMs);
    btnFullscreen_->setIcon(IconPainter::expand);
    applyState();
    emit fullscreenChanged(false);
}

bool PlayerShell::dismissOverlay() {
    if (flyout_ != nullptr && flyout_->isVisible()) {
        flyout_->close();
        return true;
    }
    if (keys_ != nullptr && keys_->isVisible()) {
        keys_->close();
        return true;
    }
    return false;
}

void PlayerShell::enterAutoHidden() {
    // Never yank the chrome away mid-gesture: an open popup or a live drag
    // means the user is interacting even though no move crossed the shell.
    if (QApplication::activePopupWidget() != nullptr) {
        hideTimer_.start(kAutohideMs);
        return;
    }
    if (scrub_->isDragging() || vol_->isDragging() || panning_ || resizingLeft_ || resizingTop_ ||
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
    const int chromeHeight = 150;
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
    const int sw = std::max(1, static_cast<int>(fit.width() * scale_));
    const int sh = std::max(1, static_cast<int>(fit.height() * scale_));
    const int sx = static_cast<int>((w - sw) / 2.0 + offset_.x());
    const int sy = static_cast<int>((h - sh) / 2.0 + offset_.y());
    surface_->setGeometry(sx, sy, sw, sh);

    // ── window controls (top-right) ────────────────────────────────
    const int wcWidth = windowControls_->sizeHint().width() > 0 ? windowControls_->sizeHint().width()
                                                                : 96;
    const int wcX = std::clamp(static_cast<int>(w * 0.945) + 4, 0, std::max(0, w - wcWidth - 4));
    windowControls_->setGeometry(wcX, std::max(6, static_cast<int>(h * 0.008)), wcWidth, 36);

    // ── flank chevrons ─────────────────────────────────────────────
    const int cy = static_cast<int>(h * 0.5) - 21;
    btnPrev_->move(static_cast<int>(w * 0.012), cy);
    btnNext_->move(w - static_cast<int>(w * 0.012) - 42, cy);

    // ── dock (bottom-centre) ───────────────────────────────────────
    const int dw = std::min(dock_->sizeHint().width() + 22, w - 24);
    dock_->setGeometry((w - dw) / 2, h - static_cast<int>(h * 0.022) - 46, dw, 46);

    // ── footers ────────────────────────────────────────────────────
    footerLeft_->adjustSize();
    footerLeft_->move(static_cast<int>(w * 0.010), h - footerLeft_->height() - 8);
    footerRight_->adjustSize();
    footerRight_->move(w - footerRight_->width() - static_cast<int>(w * 0.010),
                       h - footerRight_->height() - 8);

    // ── overlays ───────────────────────────────────────────────────
    hud_->adjustSize();
    hud_->move(w - hud_->width() - 14, static_cast<int>(h * 0.30));
    center_->move((w - 74) / 2, (h - 74) / 2);
    if (toast_->isVisible()) {
        toast_->move((w - toast_->width()) / 2, static_cast<int>(h * 0.80));
    }

    if (showHud_ && !hudHidden_) {
        hud_->move(w - hud_->width() - 14, static_cast<int>(h * 0.30));
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  ZOOM & PAN
// ═════════════════════════════════════════════════════════════════════════════

void PlayerShell::resetTransform() {
    scale_ = 1.0;
    offset_ = QPointF();
    applyState();
}

QPointF PlayerShell::clampOffset(double scale, const QPointF& offset) const {
    if (scale <= 1.001) return QPointF();
    const QSize fit = fitted();
    const double sw = fit.width() * scale;
    const double sh = fit.height() * scale;
    const double lx = std::max(0.0, (sw - width()) / 2 + 60);
    const double ly = std::max(0.0, (sh - height()) / 2 + 60);
    return {std::clamp(offset.x(), -lx, lx), std::clamp(offset.y(), -ly, ly)};
}

void PlayerShell::zoomAt(double target, const QPointF& anchor) {
    const QPointF a = anchor.isNull() ? QPointF(width() / 2.0, height() / 2.0) : anchor;
    const double s0 = scale_;
    const QPointF o0 = offset_;
    const double s1 = std::clamp(target, kZoomMin, kZoomMax);
    if (std::abs(s1 - s0) < 1e-9) return;

    // Keep the point under the anchor pinned while the scale changes.
    const double cx = a.x() - width() / 2.0;
    const double cy = a.y() - height() / 2.0;
    const double px = (cx - o0.x()) / s0;
    const double py = (cy - o0.y()) / s0;
    offset_ = clampOffset(s1, QPointF(cx - px * s1, cy - py * s1));
    scale_ = s1;
    applyState();
}

void PlayerShell::zoomBy(double factor, const QPointF& anchor) { zoomAt(scale_ * factor, anchor); }

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
    lblFile_->setText(name);
    lblOrigin_->setText(QStringLiteral("%1 · %2")
                            .arg(origin_,
                                 QDateTime::currentDateTime().toString(QStringLiteral("dd.MM.yyyy 'о' HH:mm"))));
    setWindowTitle(QStringLiteral("%1 · Media Lightbox").arg(name));
    position_ = 0.0;
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
    btnPlay_->setIcon(playing ? IconPainter::pause : IconPainter::play);
    center_->setKind(playing ? CenterGlyph::Kind::None : CenterGlyph::Kind::Play);
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
    lblElapsed_->setText(formatTime(position_));
    lblRemaining_->setText(formatSigned(std::max(0.0, duration_ - position_)));
    if (duration_ > 0 && !scrub_->isDragging()) {
        scrub_->setValueSilent(position_ / duration_);
    }
    if (showHud_ && !hudHidden_) {
        hud_->updateData(stateName(),
                         {{QStringLiteral("scale"), QString::number(scale_, 'f', 2)},
                          {QStringLiteral("offset"), QStringLiteral("%1, %2")
                                                       .arg(static_cast<int>(offset_.x()))
                                                       .arg(static_cast<int>(offset_.y()))},
                          {QStringLiteral("cursor"), cursorName(state(), panning_)},
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

void PlayerShell::openSettings() {
    flyout_->setRate(rate_);
    flyout_->setQuality(quality_);
    flyout_->setQualityOptions(qualityOptions_);
    flyout_->popupNear(btnSettings_->mapToGlobal(QPoint(btnSettings_->width(), 0)));
}

void PlayerShell::openKeys() { keys_->popupNear(btnKeys_->mapToGlobal(QPoint(0, 0))); }

void PlayerShell::openMenu() {
    QMenu menu(this);
    menu.setFont(uiFont(10));
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background: rgba(35,39,45,246); color: rgba(255,255,255,200);"
        "  border: 1px solid rgba(255,255,255,30); border-radius: 8px; padding: 6px; }"
        "QMenu::item { padding: 6px 22px; border-radius: 5px; }"
        "QMenu::item:selected { background: rgba(255,255,255,26); }"
        "QMenu::separator { height: 1px; background: rgba(255,255,255,25); margin: 4px 8px; }"));
    menu.addAction(QStringLiteral("Копіювати посилання"),
                   this, [this] { toast_->flash(QStringLiteral("URL copied")); });
    menu.addAction(QStringLiteral("Відкрити у папці"), this,
                   [this] { toast_->flash(path_.isEmpty() ? QStringLiteral("—") : path_); });
    menu.addAction(QStringLiteral("Скинути масштаб  (R)"), this, &PlayerShell::resetTransform);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Вихід  (Esc)"), this, &PlayerShell::exitViewer);
    menu.exec(btnMore_->mapToGlobal(QPoint(0, btnMore_->height())));
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
            if (fullscreen_) {
                exitFullscreen();
            } else {
                enterFullscreen();
            }
            break;
        case Qt::Key_M:
            toggleMute();
            break;
        case Qt::Key_Escape:
            if (flyout_->isVisible()) {
                flyout_->close();
            } else if (keys_->isVisible()) {
                keys_->close();
            } else if (fullscreen_) {
                exitFullscreen();
            } else if (std::abs(scale_ - 1.0) > 0.005) {
                resetTransform();
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
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            zoomBy(1 + kZoomStep);
            break;
        case Qt::Key_Minus:
            zoomBy(1 - kZoomStep);
            break;
        case Qt::Key_R:
        case Qt::Key_0:
            resetTransform();
            break;
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

void PlayerShell::wheelEvent(QWheelEvent* event) {
    wake();
    const int delta = event->angleDelta().y();
    if (delta == 0) return;
    zoomBy(1 + kZoomStep * (delta > 0 ? 1 : -1), event->position());
    event->accept();
}

void PlayerShell::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (scale_ > 1.02) {
        resetTransform();
    } else {
        zoomAt(kZoomDoubleClick, event->position());
    }
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
    if (event->button() != Qt::LeftButton) return;

    if (hitEdges(event->position())) {
        dragGeometry_ = geometry();
        dragOrigin_ = event->globalPosition().toPoint();
        grabPointer();
        event->accept();
        return;
    }

    press_ = event->position();
    pressOffset_ = offset_;
    moved_ = false;
    panning_ = false;
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
    if (scale_ <= 1.001) return;  // pan is inactive at fit scale
    panning_ = true;
    offset_ = clampOffset(scale_, pressOffset_ + QPointF(dx, dy));
    layoutChrome();
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
    const bool wasPanning = panning_;
    panning_ = false;
    if (wasPanning) {
        applyState();
    } else {
        setStateCursor();
        if (!moved_ && event->button() == Qt::LeftButton) onPlayClicked();
    }
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

    // Floating overlay while zoomed.
    if (state() == State::ZoomInPan && !hudHidden_) {
        const QFont f = monoFont(8);
        const QString text =
            QStringLiteral("ZOOM_IN_PAN · drag to pan · double-click to reset");
        const double tw = textWidth(f, text) + 22;
        const double x = (width() - tw) / 2;
        const double y = height() * 0.055;
        paintRounded(p, QRectF(x, y, tw, 20), 10, QColor(0, 0, 0, 135));
        p.setFont(f);
        p.setPen(w55());
        p.drawText(QRectF(x, y, tw, 20), Qt::AlignCenter, text);
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
    panning_ = false;
    resizingLeft_ = resizingTop_ = resizingRight_ = resizingBottom_ = false;
    hideTimer_.stop();
    flyout_->close();
    keys_->close();
    QWidget::closeEvent(event);
}

}  // namespace shell
}  // namespace mplayer
