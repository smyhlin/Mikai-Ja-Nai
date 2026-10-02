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

#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "PresetShortcuts.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QEasingCurve>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegion>
#include <QResizeEvent>
#include <QScreen>
#include <QSet>
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

constexpr int kSeriesWidth = 190;
constexpr int kStripHeight = 30;
constexpr int kStatusMs = 1900;
constexpr int kStatusLabelWidth = 560;
constexpr int kStripMargin = 8;

constexpr int kPipSeekStepSeconds = 10;
constexpr int kPipCornerRadiusPx = 16;
// The gap a parked float keeps from the work-area edge: far enough that a
// maximized window does not look like it is clipping it, close enough that
// the controls at the edges are reachable.
constexpr int kPipBoundsOffset = 24;
constexpr int kPipBarFadeMs = 160;
constexpr int kPipBarRevealHoldMs = 1600;
constexpr int kPipDragThresholdPx = 6;
constexpr int kPipCenterButtonSize = 72;
constexpr int kPipCenterIconSize = 30;
constexpr int kPipFieldHeight = 28;
// The level pill is always this size: the mute button, a gap, and a slider wide
// enough to aim at. It was built to grow on hover, which resized the window
// content under the pointer on every enter and leave.
constexpr int kPipVolumeWidth = 96;
constexpr int kPipChipWidth = kPipFieldHeight + 8 + kPipVolumeWidth + 10;
constexpr int kPipTimelineHeight = 4;
constexpr int kPipTimelineHandle = 12;

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

QString pipQss() {
    return QStringLiteral(
               "QWidget#pipOverlay { background: transparent; border: none; }"
               "QLabel#pipElapsed, QLabel#pipRemaining {"
               "  background: transparent; color: #ffffff; font-size: 11px; font-weight: 600;"
               "  font-family: monospace; }"
               "QPushButton#pipPlay {"
               "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
               "    stop:0 rgba(0, 0, 0, 0.42), stop:1 rgba(0, 0, 0, 0.58));"
               "  border: 1px solid rgba(255, 255, 255, 0.26);"
               "  border-radius: %1px; padding: 0;"
               "  min-width: %2px; max-width: %2px;"
               "  min-height: %2px; max-height: %2px; }"
               "QPushButton#pipPlay:hover {"
               "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
               "    stop:0 rgba(0, 0, 0, 0.60), stop:1 rgba(0, 0, 0, 0.76));"
               "  border-color: rgba(255, 255, 255, 0.46); }"
               "QPushButton#pipPlay:pressed {"
               "  background: rgba(0, 0, 0, 0.80); }"
               "QPushButton#pipMute, QPushButton#pipRestore, QPushButton#pipClose {"
               "  background: rgba(0, 0, 0, 0.45);"
               "  border: 1px solid rgba(255, 255, 255, 0.15);"
               "  border-radius: 6px; padding: 0; }"
               "QPushButton#pipMute:hover, QPushButton#pipRestore:hover, QPushButton#pipClose:hover {"
               "  background: rgba(0, 0, 0, 0.75);"
               "  border-color: rgba(255, 255, 255, 0.35); }"
               "QWidget#pipVolumeChip { background: transparent; border: none; }"
               "QSlider#pipVolume { background: transparent; }"
               "QSlider#pipVolume::groove:horizontal {"
               "  height: 4px; background: rgba(255, 255, 255, 0.22); border-radius: 2px; }"
               "QSlider#pipVolume::sub-page:horizontal {"
               "  background: rgba(255, 255, 255, 0.92); border-radius: 2px; }"
               "QSlider#pipVolume::handle:horizontal {"
               "  background: #ffffff; width: 12px; height: 12px;"
               "  margin: -4px 0; border-radius: 6px; }"
               "QSlider#pipVolume::groove:horizontal:disabled { background: rgba(255, 255, 255, 0.10); }"
               "QSlider#pipScrub { background: transparent; }"
               "QSlider#pipScrub::groove:horizontal {"
               "  height: 4px; background: rgba(255, 255, 255, 0.25); border-radius: 2px; }"
               "QSlider#pipScrub::sub-page:horizontal { background: #5865F2; border-radius: 2px; }"
               "QSlider#pipScrub::handle:horizontal {"
               "  background: #ffffff; width: 10px; height: 10px;"
               "  margin: -3px 0; border-radius: 5px; }")
        .arg(kPipCenterButtonSize / 2)
        .arg(kPipCenterButtonSize);
}

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

/// Seconds as m:ss, gaining an hours field only when it is needed.
QString pipFormatSeconds(double value) {
    const qint64 total = static_cast<qint64>(std::max(0.0, value));
    const qint64 hours = total / 3600;
    const qint64 rest = total % 3600;
    const qint64 minutes = rest / 60;
    const qint64 seconds = rest % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

/// Rasterises one painted glyph into an icon. IconPainter already draws into a
/// 24-unit grid, so this is a straight scale-and-tint with no SVG pipeline.
QIcon glyphIcon(void (*glyph)(QPainter&, qreal, qreal, qreal, const QColor&), qreal size) {
    const QColor normal(kText);
    const QColor disabled(kIconDisabled);
    QIcon icon;
    for (const QColor& color : {normal, disabled}) {
        QPixmap pixmap(QSize(static_cast<int>(size), static_cast<int>(size)));
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        glyph(painter, 0.0, 0.0, size, color);
        icon.addPixmap(pixmap, color == normal ? QIcon::Normal : QIcon::Disabled);
    }
    return icon;
}

/// The preset bindings that are actually installed, keyed by the combination
/// they hold. The native toggle asks the same question the preset loader asks,
/// through the same shared collision check, so a native binding can never take
/// a key a preset is already using.
QMap<QString, ShortcutClaim> installedPresetClaims() {
    QMap<QString, ShortcutClaim> claims;
    QSettings settings;
    settings.beginGroup(QLatin1String(kPresetShortcutSettingsGroup));
    QSet<QKeySequence> claimed;
    for (const anime4k::ModeInfo& info : anime4k::availableModes()) {
        const QString stored = settings.value(info.id).toString();
        if (stored.isEmpty()) {
            continue;
        }
        const QKeySequence sequence =
            QKeySequence::fromString(stored, QKeySequence::PortableText);
        if (sequence.isEmpty() || isReservedPresetShortcut(sequence) ||
            claimed.contains(sequence)) {
            continue;
        }
        claimed.insert(sequence);
        ShortcutClaim claim;
        claim.ownerId = info.id;
        claim.ownerLabel = info.label;
        claim.kind = ShortcutOwnerKind::Preset;
        claims.insert(shortcutClaimKey(sequence), claim);
    }
    settings.endGroup();
    return claims;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  PipFloat
// ═══════════════════════════════════════════════════════════════════════════

PipFloat::PipFloat(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setObjectName(QStringLiteral("playerPip"));
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    surface_ = new VideoSurface(this);
    surface_->setObjectName(QStringLiteral("pipVideo"));
    surface_->setFocusPolicy(Qt::NoFocus);
    surface_->setMouseTracking(true);

    buildChrome();

    overlayEffect_ = new QGraphicsOpacityEffect(overlay_);
    overlayEffect_->setOpacity(1.0);
    overlay_->setGraphicsEffect(overlayEffect_);

    overlayFade_ = new QVariantAnimation(this);
    overlayFade_->setDuration(kPipBarFadeMs);
    overlayFade_->setEasingCurve(QEasingCurve::OutCubic);
    connect(overlayFade_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { setOverlayOpacity(value.toDouble()); });

    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    connect(hideTimer_, &QTimer::timeout, this, &PipFloat::hideControls);

    connect(surface_, &VideoSurface::durationChanged, this, [this](double seconds) {
        if (pendingSeek_ >= 0.0 && seconds > 0.0) {
            const double at = pendingSeek_;
            pendingSeek_ = -1.0;
            surface_->seek(at);
        }
    });
    connect(surface_, &VideoSurface::errorOccurred, this,
            [this](const QString&) { revealControls(); });

    // The float's clock follows its OWN core. It used to be fed from the host
    // on the main surface, which is stopped while PiP is up - so the counter
    // froze on the moment PiP opened and never moved again.
    connect(surface_, &VideoSurface::positionChanged, this,
            [this](double seconds) { setTime(seconds, surface_->duration()); });
    // The scrubber's range belongs to the length, so it is set where the
    // length arrives rather than on every position tick.
    connect(surface_, &VideoSurface::durationChanged, this, [this](double seconds) {
        if (seconds > 0.0) {
            scrub_->setEnabled(true);
            scrub_->setRange(0, static_cast<int>(std::lround(seconds * 1000.0)));
        } else {
            scrub_->setEnabled(false);
        }
        setTime(surface_->position(), seconds);
    });
    connect(surface_, &VideoSurface::playingChanged, this,
            [this](bool playing) { setPlaying(playing); });
}

void PipFloat::buildChrome() {
    overlay_ = new QWidget(this);
    overlay_->setObjectName(QStringLiteral("pipOverlay"));
    overlay_->setAttribute(Qt::WA_TranslucentBackground);
    overlay_->setFocusPolicy(Qt::StrongFocus);
    overlay_->setMouseTracking(true);
    overlay_->setStyleSheet(pipQss());
    overlay_->installEventFilter(this);
    // The overlay is full-rect and forwards input to the float for drag/move
    // and click-to-pause.
    overlay_->setFocusProxy(this);

    // A container, not a button: the mute button and the level it governs sit
    // in one pill. It was built to grow on hover, and that read as lag: the
    // pill resized under the pointer every time it was entered and left, and
    // the whole strip jumped each time. A fixed pill is one shape, always.
    volumeChip_ = new QWidget(overlay_);
    volumeChip_->setObjectName(QStringLiteral("pipVolumeChip"));
    volumeChip_->setFocusPolicy(Qt::NoFocus);
    volumeChip_->setMouseTracking(true);
    auto* chipLayout = new QHBoxLayout(volumeChip_);
    chipLayout->setContentsMargins(4, 4, 4, 4);
    chipLayout->setSpacing(6);

    muteButton_ = buildButton(QStringLiteral("pipMute"), tr("Mute audio"),
                              kPipFieldHeight, volumeChip_);
    muteButton_->setIcon(glyphIcon(&IconPainter::volume, 16));
    connect(muteButton_, &QPushButton::clicked, this, [this] {
        setMuted(!muted_);
        revealControls();
        emit muteChanged(muted_);
    });
    chipLayout->addWidget(muteButton_);

    volumeSlider_ = new QSlider(Qt::Horizontal, volumeChip_);
    volumeSlider_->setObjectName(QStringLiteral("pipVolume"));
    volumeSlider_->setFocusPolicy(Qt::NoFocus);
    volumeSlider_->setMouseTracking(true);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setFixedWidth(kPipVolumeWidth);
    volumeSlider_->setValue(static_cast<int>(std::lround(volume_ * 100.0)));
    connect(volumeSlider_, &QSlider::sliderPressed, this, [this] { draggingVolume_ = true; });
    connect(volumeSlider_, &QSlider::sliderReleased, this, [this] {
        draggingVolume_ = false;
        setVolume(volumeSlider_->value() / 100.0);
        emit volumeChanged(volume_);
        revealControls();
    });
    // The level reaches mpv at the release, not on every tick. A QSlider emits
    // valueChanged per pixel of travel, and each one was a synchronous property
    // write plus a groove relayout; on a slow core that queue is what reads as
    // lag. The handle still tracks the pointer - that is the widget painting
    // itself - and the sound lands the moment the button is let go.
    connect(volumeSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (!draggingVolume_) {
            return;
        }
        volume_ = std::clamp(value / 100.0, 0.0, 1.0);
    });
    chipLayout->addWidget(volumeSlider_);

    volumeChip_->setFixedSize(kPipChipWidth, kPipFieldHeight + 8);

    restoreButton_ =
        buildButton(QStringLiteral("pipRestore"), tr("Return to the window"),
                    kPipFieldHeight);
    restoreButton_->setIcon(glyphIcon(&IconPainter::expand, 15));
    connect(restoreButton_, &QPushButton::clicked, this, &PipFloat::restoreRequested);

    closeButton_ = buildButton(QStringLiteral("pipClose"), tr("Close (Esc)"),
                               kPipFieldHeight);
    closeButton_->setIcon(glyphIcon(&IconPainter::close, 15));
    connect(closeButton_, &QPushButton::clicked, this, &PipFloat::closeRequested);

    playButton_ = new QPushButton(overlay_);
    playButton_->setObjectName(QStringLiteral("pipPlay"));
    playButton_->setFixedSize(kPipCenterButtonSize, kPipCenterButtonSize);
    playButton_->setIcon(glyphIcon(&IconPainter::pause, kPipCenterIconSize));
    playButton_->setToolTip(tr("Pause (Space)"));
    playButton_->setAccessibleName(tr("Pause"));
    playButton_->setFocusPolicy(Qt::NoFocus);
    playButton_->setMouseTracking(true);
    connect(playButton_, &QPushButton::clicked, surface_, &VideoSurface::togglePause);

    elapsed_ = new QLabel(QStringLiteral("0:00"), overlay_);
    elapsed_->setObjectName(QStringLiteral("pipElapsed"));
    elapsed_->setMouseTracking(true);

    remaining_ = new QLabel(QStringLiteral("--:--"), overlay_);
    remaining_->setObjectName(QStringLiteral("pipRemaining"));
    remaining_->setMouseTracking(true);

    scrub_ = new QSlider(Qt::Horizontal, overlay_);
    scrub_->setObjectName(QStringLiteral("pipScrub"));
    scrub_->setFocusPolicy(Qt::NoFocus);
    scrub_->setMouseTracking(true);
    scrub_->setRange(0, 1);
    connect(scrub_, &QSlider::sliderPressed, this, [this] {
        scrubbing_ = true;
        revealControls();
    });
    connect(scrub_, &QSlider::sliderReleased, this, [this] {
        scrubbing_ = false;
        revealControls();
        // The float acts on its OWN core. Routing this to the host used to
        // seek the main player, hidden behind the float, so the scrubber
        // appeared to do nothing at all.
        surface_->seek(scrub_->value() / 1000.0);
    });
    // Click-to-seek. The overlay claims every press before the slider sees it,
    // so a click on the timeline fell through to the picture and only
    // toggled playback. An event filter is the only way in: QSlider handles
    // its own press in a protected override, so there is no signal for it.
    scrub_->installEventFilter(this);

    installEventFilter(this);
    surface_->installEventFilter(this);
    const QVector<QWidget*> watched = {elapsed_,     remaining_,   scrub_,
                                       playButton_,  muteButton_, restoreButton_,
                                       closeButton_, volumeChip_, volumeSlider_};
    for (QWidget* widget : watched) {
        widget->installEventFilter(this);
    }
}

QPushButton* PipFloat::buildButton(const QString& name, const QString& hint, qreal size,
                                   QWidget* parent) {
    auto* button = new QPushButton(parent != nullptr ? parent : overlay_);
    button->setObjectName(name);
    button->setFixedSize(QSize(static_cast<int>(size), static_cast<int>(size)));
    button->setToolTip(hint);
    button->setAccessibleName(hint);
    button->setFocusPolicy(Qt::NoFocus);
    button->setMouseTracking(true);
    return button;
}

void PipFloat::openStream(const QString& url, double position) {
    if (url.isEmpty()) {
        return;
    }
    pendingSeek_ = position;
    surface_->open(url);
    fitSurfaces();
}

void PipFloat::closeStream() {
    pendingSeek_ = -1.0;
    surface_->close();
}

void PipFloat::setPlaying(bool playing) {
    playButton_->setIcon(
        glyphIcon(playing ? &IconPainter::pause : &IconPainter::play, kPipCenterIconSize));
    const QString hint = playing ? tr("Pause (Space)")
                                 : tr("Play (Space)");
    playButton_->setToolTip(hint);
    playButton_->setAccessibleName(hint);
}

void PipFloat::setTime(double position, double duration) {
    // Display only: nothing is emitted, so answering with a seek could loop.
    // The position is shown whenever it is known. Blanking it merely because
    // the length is not - a live stream, or a clip still reporting zero -
    // threw away the one number the viewer can actually read.
    if (position > 0.0) {
        elapsed_->setText(pipFormatSeconds(position));
    } else {
        elapsed_->setText(QStringLiteral("0:00"));
    }
    remaining_->setText(duration > 0.0
                            ? QStringLiteral("-%1").arg(pipFormatSeconds(std::max(0.0, duration - position)))
                            : QStringLiteral("--:--"));
    // An unknown length leaves the scrubber disabled rather than collapsing it
    // to a 1 ms range, where every drag reads as a jump back to the start.
    if (duration > 0.0) {
        scrub_->setEnabled(true);
        // The range is set by the durationChanged handler, not here: the
        // length does not change between position ticks, and setRange re-lays
        // out the styled groove on every call.
        if (!scrubbing_) {
            const double clamped = std::clamp(position, 0.0, duration);
            scrub_->setValue(static_cast<int>(std::lround(clamped * 1000.0)));
        }
    } else {
        scrub_->setEnabled(false);
    }
}

double PipFloat::position() const { return surface_ != nullptr ? surface_->position() : 0.0; }
double PipFloat::duration() const { return surface_ != nullptr ? surface_->duration() : 0.0; }
bool PipFloat::isPlaying() const { return surface_ != nullptr && surface_->isPlaying(); }

void PipFloat::setMuted(bool muted) {
    muted_ = muted;
    // The float owns a second mpv core. Recording the flag and swapping the
    // glyph changed nothing audible, because nothing ever told that core: the
    // button muted the icon, not the sound. The volume rides along, because
    // a muted core at level 0 is a mute the slider cannot then undo.
    surface_->setMuted(muted);
    surface_->setVolume(volume_ * 100.0);
    const QString hint = muted ? tr("Unmute audio") : tr("Mute audio");
    muteButton_->setIcon(glyphIcon(muted ? &IconPainter::mute : &IconPainter::volume, 16));
    muteButton_->setToolTip(hint);
    muteButton_->setAccessibleName(hint);
    updateVolumeChip();
}

void PipFloat::setVolume(double volume) {
    volume_ = std::clamp(volume, 0.0, 1.0);
    surface_->setVolume(volume_ * 100.0);
    if (volume_ > 0.0 && muted_) {
        // Dragging the level up on a muted core is how a viewer un-mutes
        // without hunting for the button, and it has to be audible.
        setMuted(false);
    }
    updateVolumeChip();
}

void PipFloat::updateVolumeChip() {
    if (volumeSlider_ == nullptr) {
        return;
    }
    // Writing the value back while the pointer is on the slider is what made
    // it feel like it was lagging: the widget was being told where the handle
    // already was, on every one of the hundreds of ticks a drag produces, and
    // each write re-lays the groove out. The slider is the source of truth
    // mid-drag; this only mirrors an outside change.
    if (!draggingVolume_) {
        volumeSlider_->setValue(static_cast<int>(std::lround(volume_ * 100.0)));
    }
}

// ── the controls come and go with the pointer ───────────────────────────

void PipFloat::setOverlayOpacity(double value) {
    overlayOpacity_ = value;
    overlayEffect_->setOpacity(overlayOpacity_);
}

void PipFloat::revealControls() {
    // Already fully revealed and nothing pending: every call below is a no-op,
    // except that setOpacity() is not one. QGraphicsEffect::setOpacity
    // invalidates the effect and forces the whole overlay - buttons, labels,
    // slider and all, under QSS - through the offscreen opacity buffer again
    // on the next paint. This runs on every mouse-move over the float, so
    // without this the controls were re-composited continuously under a
    // pointer that was not going anywhere.
    if (hovered_ && overlayOpacity_ >= 1.0 && overlayFade_->state() != QAbstractAnimation::Running
        && !hideTimer_->isActive()) {
        return;
    }
    // The reveal is instant on purpose: a fade in makes the controls feel late
    // to a pointer that is already on the picture.
    hideTimer_->stop();
    overlayFade_->stop();
    setOverlayOpacity(1.0);
    // A hover can reach a float the user never activated, so the controls have
    // to retire again on their own rather than wait for a focus event that may
    // never arrive - but never while the pointer is still on them.
    if (!hovered_) {
        hideTimer_->start(kPipBarRevealHoldMs);
    }
}

void PipFloat::hideControls() {
    // Never fade out from under a pointer that is still on the float: the
    // controls would vanish mid-reach.
    if (hovered_ || overlayOpacity_ <= 0.01) {
        return;
    }
    overlayFade_->stop();
    overlayFade_->setStartValue(overlayOpacity_);
    overlayFade_->setEndValue(0.0);
    overlayFade_->start();
}

bool PipFloat::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::Wheel:
            revealControls();
            break;
        default:
            break;
    }
    // Win32 delivers activation to the top level, and a changeEvent override
    // never sees it, so the float watches its own events.
    if (watched == this) {
        switch (event->type()) {
            case QEvent::Enter:
            case QEvent::HoverEnter:
                hovered_ = true;
                revealControls();
                break;
            case QEvent::Leave:
            case QEvent::HoverLeave:
                hovered_ = false;
                // Linger a moment after the pointer goes, so a click that ends
                // outside does not make the controls blink.
                hideTimer_->start(kPipBarRevealHoldMs);
                break;
            case QEvent::WindowActivate:
                revealControls();
                break;
            case QEvent::WindowDeactivate:
                hideTimer_->stop();
                hideControls();
                break;
            default:
                break;
        }
    } else if (watched == scrub_) {
        if (event->type() == QEvent::MouseButtonPress) {
            if (auto* press = dynamic_cast<QMouseEvent*>(event)) {
                timelinePress(press);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            draggingTimeline_ = false;
            scrubbing_ = false;
        }
    } else if (watched == overlay_) {
        // claim a press; the children never let one past the overlay.
        if (auto* press = dynamic_cast<QMouseEvent*>(event)) {
            switch (event->type()) {
                case QEvent::MouseButtonPress:
                    pointerPress(press);
                    return true;
                case QEvent::MouseMove:
                    pointerMove(press);
                    return true;
                case QEvent::MouseButtonRelease:
                    pointerRelease(press);
                    return true;
                default:
                    break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PipFloat::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    fitSurfaces();
    revealControls();
}

void PipFloat::closeEvent(QCloseEvent* event) {
    // A close from the window manager must not leave a live VideoSurface behind
    // a deleted float: the stream is stopped before the host tears it down.
    closeStream();
    QWidget::closeEvent(event);
}

// ── the gesture on the picture ───────────────────────────────────────────

void PipFloat::pointerPress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    // Both anchors are taken in screen space: the pointer position it was
    // grabbed at, and where the window itself was sitting when it was.
    pressGlobal_ = event->globalPosition();
    pressWindow_ = pos();
    pressed_ = true;
    dragging_ = false;
}

void PipFloat::pointerMove(QMouseEvent* event) {
    // A drag that started on the timeline scrubs, no matter where the pointer
    // has wandered to since: the overlay sees the move first, and without
    // this the window would start sliding out from under a scrubbing hand.
    if (draggingTimeline_) {
        timelineSeekTo(event->position().x());
        return;
    }
    if (!pressed_ || !(event->buttons() & Qt::LeftButton)) {
        return;
    }
    const QPointF delta = event->globalPosition() - pressGlobal_;
    if (!dragging_) {
        if (std::abs(delta.x()) + std::abs(delta.y()) <= kPipDragThresholdPx) {
            return;
        }
        dragging_ = true;
    }
    // The window is placed from where it started plus where the pointer has come
    // to, not nudged by the last event: both operands are screen positions, so
    // nothing rounds twice and the float stays under the cursor.
    move(pressWindow_ + QPoint(static_cast<int>(std::lround(delta.x())),
                               static_cast<int>(std::lround(delta.y()))));
}

void PipFloat::timelineSeekTo(double x) {
    const double duration = surface_->duration();
    if (duration <= 0.0 || scrub_ == nullptr) {
        return;
    }
    // The handle is 10px wide, so the usable groove is inset by half of it on
    // both ends: a click at the very edge of the widget is the very start,
    // not a half-handle past it.
    const double span = std::max(1.0, static_cast<double>(scrub_->width()) - kPipTimelineHandle);
    const double at = std::clamp((x - kPipTimelineHandle / 2.0) / span, 0.0, 1.0);
    const double seconds = at * duration;
    scrubbing_ = true;
    scrub_->setValue(static_cast<int>(std::lround(seconds * 1000.0)));
    // The float acts on its OWN core: seeking the hidden main player here
    // moved a video nobody can see.
    surface_->seek(seconds);
}

void PipFloat::timelinePress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    revealControls();
    draggingTimeline_ = true;
    // The press is taken in the float's coordinates: the overlay is full-rect,
    // so the slider and the window share an origin.
    timelineSeekTo(event->position().x());
}
void PipFloat::pointerRelease(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const bool dragged = dragging_;
    pressed_ = false;
    dragging_ = false;
    if (draggingTimeline_) {
        draggingTimeline_ = false;
        scrubbing_ = false;
        revealControls();
        return;
    }
    if (dragged) {
        // Land on the nearest corner. Parked wherever the finger left it, a
        // float ends up half over the taskbar or off the usable area.
        snapToCorner();
        return;
    }
    surface_->togglePause();
}

// ── layout ───────────────────────────────────────────────────────────────

void PipFloat::moveToCorner(QScreen* target) {
    if (target == nullptr) {
        return;
    }
    // availableGeometry, not geometry: the taskbar and the docked app bar are
    // not places a float can be dragged back from, so a window parked on the
    // work area is one the viewer cannot reach.
    const QRect area = target->availableGeometry();
    move(area.right() - width() - kPipBoundsOffset + 1,
         area.bottom() - height() - kPipBoundsOffset + 1);
}

void PipFloat::snapToCorner() {
    // Snapping is a one-off on release, not a live guide: the float follows
    // the pointer exactly while it is down, and lands on the nearest corner
    // when it comes up. A live guide would be a second thing to look at while
    // dragging the thing you came to move.
    QScreen* target = QGuiApplication::screenAt(frameGeometry().center());
    if (target == nullptr) {
        return;
    }
    const QRect area = target->availableGeometry();
    const QPoint centre = frameGeometry().center();
    const bool left = centre.x() < area.center().x();
    const bool top = centre.y() < area.center().y();
    const int x = left ? area.left() + kPipBoundsOffset
                        : area.right() - width() - kPipBoundsOffset + 1;
    const int y = top ? area.top() + kPipBoundsOffset
                       : area.bottom() - height() - kPipBoundsOffset + 1;
    if (pos() == QPoint(x, y)) {
        return;
    }
    // Animated because a float that jumps reads as a glitch; short, because it
    // is the last thing that happened and the viewer is already looking away.
    auto* anim = new QPropertyAnimation(this, "pos", this);
    anim->setDuration(120);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(pos());
    anim->setEndValue(QPoint(x, y));
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}
void PipFloat::fitSurfaces() {
    surface_->setGeometry(rect());
    overlay_->setGeometry(rect());
    overlay_->raise();

    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0) {
        return;
    }

    // Top-left: the volume pill. The chip is moved, not the button inside it -
    // placing the button directly left the pill wherever its layout last put it,
    // so the two drew on top of each other.
    if (volumeChip_ != nullptr) {
        volumeChip_->move(12, 12);
    }

    // Top-right: close button (right: w - 12 - width), restore button (left of close)
    if (closeButton_ != nullptr) {
        closeButton_->move(w - 12 - closeButton_->width(), 12);
    }
    if (restoreButton_ != nullptr && closeButton_ != nullptr) {
        restoreButton_->move(closeButton_->x() - 8 - restoreButton_->width(), 12);
    }

    // Center: play button
    if (playButton_ != nullptr) {
        playButton_->move((w - playButton_->width()) / 2, (h - playButton_->height()) / 2);
    }

    // Bottom: scrubber slider at very bottom (~5px above edge)
    const int scrubH = 10;
    const int scrubY = h - scrubH - 5;
    if (scrub_ != nullptr) {
        scrub_->setGeometry(10, scrubY, qMax(0, w - 20), scrubH);
    }

    // Bottom time labels: 10px from bottom / right above scrubber, 12px padding from sides
    const int labelY = scrubY - 18;
    if (elapsed_ != nullptr) {
        elapsed_->adjustSize();
        elapsed_->move(12, labelY);
    }
    if (remaining_ != nullptr) {
        remaining_->adjustSize();
        remaining_->move(qMax(0, w - 12 - remaining_->width()), labelY);
    }
}

void PipFloat::applyCornerMask() {
    // Round the window itself, not just its background: a styled border-radius
    // only clips the widget's own painting, and the video is a child that
    // paints square corners straight over the rounded background otherwise.
    if (width() <= 0 || height() <= 0) {
        return;
    }
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), kPipCornerRadiusPx, kPipCornerRadiusPx);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void PipFloat::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    applyCornerMask();
    fitSurfaces();
}

// ── keyboard ─────────────────────────────────────────────────────────────

bool PipFloat::handleKey(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Left:
            surface_->seek(surface_->position() - kPipSeekStepSeconds);
            break;
        case Qt::Key_Right:
            surface_->seek(surface_->position() + kPipSeekStepSeconds);
            break;
        case Qt::Key_Space:
            surface_->togglePause();
            break;
        case Qt::Key_Escape:
            emit closeRequested();
            break;
        default:
            return false;
    }
    revealControls();
    return true;
}

void PipFloat::keyPressEvent(QKeyEvent* event) {
    if (!handleKey(event)) {
        QWidget::keyPressEvent(event);
    }
}

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
    loadNativeShortcuts();
    refreshNativeInputs();

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
    if (statusTimer_) {
        statusTimer_->stop();
    }
    if (audioPopup_) {
        audioPopup_->close();
    }
    if (subtitlesPopup_) {
        subtitlesPopup_->close();
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
    statusLabel_->setMaximumWidth(kStatusLabelWidth);
    row->addWidget(statusLabel_, 1);

    retryButton_ =
        makeChip(statusPanel_, QStringLiteral("playerRetryButton"), tr("Retry"));
    retryButton_->setToolTip(tr("Retry playback"));
    retryButton_->setAccessibleName(tr("Retry playback"));
    connect(retryButton_, &QPushButton::clicked, this, &PlayerDialog::retryPlayback);
    row->addWidget(retryButton_, 0);
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
    connect(shell_, &PlayerShell::compareRequested, this, &PlayerDialog::openCompareWindow);
    connect(shell_, &PlayerShell::stateChanged, this, &PlayerDialog::onShellStateChanged);
    connect(shell_, &PlayerShell::audioRequested, this, &PlayerDialog::openAudioMenu);
    connect(shell_, &PlayerShell::subtitlesRequested, this, &PlayerDialog::openSubtitlesMenu);
    connect(shell_, &PlayerShell::qualityRequested, this, &PlayerDialog::onQualityPicked);
    connect(shell_, &PlayerShell::fullscreenChanged, this,
            &PlayerDialog::onShellFullscreenChanged);
    // The menu's native rows take the same path a settings control and the
    // toggle shortcut take: one requested-change handler, so the three can
    // never disagree about what was asked for.
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
    QSet<QKeySequence> claimed;
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
        if (sequence.isEmpty() || isReservedPresetShortcut(sequence)) {
            continue;
        }
        if (claimed.contains(sequence)) {
            continue;
        }
        claimed.insert(sequence);

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

void PlayerDialog::loadNativeShortcuts() {
    // The toggle is a *stored* binding owned by the native preference, and this
    // only installs it. The default is offered when nothing is stored, and even
    // then it is installed only if the shared collision check accepts it -
    // against the reserved player keys and every preset binding already live.
    const native::NativePreferences preferences = native::loadNativePreferences();
    const QString stored = native::menu::toggleShortcut(preferences);
    if (stored.isEmpty()) {
        return;
    }
    const QKeySequence sequence =
        QKeySequence::fromString(stored, QKeySequence::PortableText);
    if (sequence.isEmpty() || isReservedPresetShortcut(sequence)) {
        return;
    }
    const ShortcutConflict conflict = findShortcutConflict(sequence, installedPresetClaims());
    if (conflict.blocked) {
        // Somebody already owns the key. The native action is simply not
        // installed; it never shadows a preset or a player key.
        return;
    }
    auto* shortcut = new QShortcut(sequence, this);
    shortcut->setContext(Qt::WindowShortcut);
    connect(shortcut, &QShortcut::activated, this, &PlayerDialog::toggleNativeProcessing);
}

void PlayerDialog::toggleNativeProcessing() {
    const native::NativePreferences preferences = native::loadNativePreferences();
    // The toggle flips the *request*. Off changes no pixels; on is persisted as
    // the request and, with no engine, reports the reason instead of pretending
    // it was applied.
    applyNativeRequest(preferences.enabled ? native::menu::Action::TurnOff
                                           : native::menu::Action::TurnOn,
                       QString());
}

void PlayerDialog::refreshNativeInputs() {
    if (shell_ == nullptr) {
        return;
    }
    // The four inputs the shell and its status HUD read, replaced as one: the
    // request, the producer availability, the runtime that was actually selected
    // and the filter's own status. None of them is invented here.
    native::menu::Inputs inputs;
    inputs.preferences = native::loadNativePreferences();
    inputs.availability = native::currentNativeAvailability();
    inputs.runtime = native::currentSelection();
    if (surface_ != nullptr) {
        inputs.status = surface_->nativeStatus();
    }
    shell_->setNativeMenuInputs(inputs);
    syncNativeStatusPolling(inputs.preferences.enabled);
}

void PlayerDialog::onNativeActionRequested(native::menu::Action action, const QString& value) {
    applyNativeRequest(action, value);
}

void PlayerDialog::onNativeStatusChanged(const native::NativeStatusView& status) {
    if (shell_ == nullptr) {
        return;
    }
    // The reader published a genuine change: amend the inputs already held
    // rather than reading the store again, and let the shell re-render the HUD
    // from them. Polling is deliberately not re-armed here - the surface is the
    // one that stops on a terminal state or an absent filter, and a call here
    // would clear that halt.
    native::menu::Inputs inputs = shell_->nativeInputs();
    inputs.status = status;
    shell_->setNativeMenuInputs(inputs);
}

void PlayerDialog::syncNativeStatusPolling(bool requested) {
    if (surface_ == nullptr) {
        return;
    }
    // Native is read only while a core exists to read from and the request is
    // on. With no core there is nothing to poll, and with the request off the
    // chip must not keep showing a chain nobody asked for.
    surface_->setNativeStatusPolling(requested && engineReady_);
}

void PlayerDialog::applyNativeRequest(native::menu::Action action, const QString& value) {
    if (action == native::menu::Action::None) {
        return;
    }

    native::NativePreferencePatch patch;
    switch (action) {
        case native::menu::Action::TurnOff:
            patch.enabled = false;
            break;
        case native::menu::Action::TurnOn:
            patch.enabled = true;
            break;
        case native::menu::Action::SetBackend: {
            const std::optional<native::Backend> backend = native::backendFromName(value);
            if (!backend.has_value()) {
                return;
            }
            patch.enabled = true;
            patch.backend = *backend;
            break;
        }
        case native::menu::Action::SetRecipe:
            if (value.isEmpty()) {
                return;
            }
            patch.enabled = true;
            patch.recipeId = value;
            break;
        case native::menu::Action::None:
            return;
    }

    QString reason;
    if (!native::saveNativePreferences(patch, &reason)) {
        // A refused save is reported as itself; nothing was changed.
        flashStatus(tr("Could not save the native preference: %1").arg(reason));
        return;
    }
    refreshNativeInputs();

    if (action == native::menu::Action::TurnOff) {
        // Off never touches the Anime4K chain, the stream or the media.
        flashStatus(tr("Native processing off · Original picture"));
        return;
    }

    const native::NativeSelectionView view = native::evaluateNativeSelection(
        native::loadNativePreferences(), native::currentNativeAvailability());
    if (view.requestAvailable) {
        // Eligible is not active: only the filter's own status snapshot may
        // report the chain as running, and the shell renders the HUD from that.
        flashStatus(tr("Native processing requested"));
        return;
    }
    // A processing reason, not a media one: no retry chip, no re-extraction, no
    // API call, no quality refetch, and the URL is untouched. The menu around
    // this row offers the remedies - switch provider or stay on Original.
    showProcessingStatus(
        view.availability.reason.isEmpty()
            ? tr("Native processing is not available in this build.")
            : tr("%1 Keeping the Original pipeline.").arg(view.availability.reason));
}

void PlayerDialog::reportNativeProcessingFailure(const QString& reason) {
    // A native preparation or engine failure is separated from a media failure
    // on purpose: it must never consume API rate budget, re-extract a signed
    // URL, or masquerade as an expired link.
    const QString message = reason.trimmed().isEmpty()
                                ? tr("Native processing failed.")
                                : reason.trimmed();
    showProcessingStatus(tr("%1 Staying on the Original picture.").arg(message));
}

void PlayerDialog::copyStreamUrl() {
    if (!directMedia_) {
        host_->requestCopyStreamUrl();
    } else {
        QGuiApplication::clipboard()->setText(url_);
    }
    flashStatus(tr("Video link copied"));
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
    // The shell drops its own mute when a volume arrives, so the mute state is
    // pushed after it or the two would fight.
    shell_->setVolume(volume_);
    shell_->setMuted(muted_);
    shell_->setRate(std::clamp(speed_, kShellRateMin, kShellRateMax));
    if (errorActive_) {
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
    // A swap carries the position, the pause and the rate across so playback
    // continues where it was instead of restarting.
    resumePending_ = resume && surface_->hasVideo();
    if (resumePending_) {
        resumePosition_ = surface_->position();
        resumePaused_ = !surface_->isPlaying();
    }
    if (height > 0) {
        currentHeight_ = height;
        shell_->setQuality(QStringLiteral("%1p").arg(height));
    }
    url_ = url;
    // open() reports false only when the engine itself could not be created, so
    // this is the one place that knows whether a retry is worth offering.
    engineReady_ = surface_->open(url);
    if (!resumePending_) {
        // Volume, mute and rate live on the core across a load, but the shell
        // and the float still need to hear the new state.
        shell_->setVolume(volume_);
        shell_->setMuted(muted_);
        shell_->setRate(std::clamp(speed_, kShellRateMin, kShellRateMax));
    }
    publishQualityOptions();
    syncPipBar();
    // A new source is a new filter session: the reader was rebound and its view
    // cleared, so the held inputs and the poll demand follow the new core rather
    // than a plan from the previous one.
    refreshNativeInputs();
    return engineReady_;
}

void PlayerDialog::restoreResumePoint() {
    resumePending_ = false;
    const double position = std::clamp(resumePosition_, 0.0, surface_->duration());
    surface_->seek(position);
    shell_->setPosition(position);
    if (resumePaused_) {
        surface_->togglePause();
        shell_->setPlaying(false);
    } else {
        shell_->setPlaying(true);
    }
}

void PlayerDialog::onShellStateChanged(const QString& state) {
    if (closed_) {
        return;
    }
    // The shell hides ALL its chrome after an idle moment, and these
    // dialog-owned panels ride along: a strip floating over a blank screen is a
    // stray panel.
    if (state == QStringLiteral("FULLSCREEN_AUTO_HIDDEN")) {
        // Popovers close with the chrome, not with fullscreen. Closing them on
        // "fullscreen" looked right and was wrong: the player is always
        // fullscreen, so every idle/hide transition killed a popover that had
        // just been opened, and the selector appeared not to work at all.
        //
        // The dialog-owned panels stay mounted: the strip, the status panel
        // and the drawer are separate top-level overlays the shell never owns,
        // so a shell broadcast about its own chrome cannot unmount them. Each
        // hides on its own rule (popovers on open, the toast on its timer, the
        // retry chip only when a new stream displaces it).
        audioPopup_->close();
        subtitlesPopup_->close();
        // The episode drawer stays. It is a layout sibling of the video, not a
        // floating panel, and the user opened it deliberately - closing it here
        // made an explicitly expanded list blink out from under someone
        // reading it, which is how "the player has no episode list" gets
        // reported. The idle timer may take the chrome; it does not get to
        // overrule that choice.
    } else {
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
    if (compare_ == nullptr) {
        compare_ = new CompareWindow(surface_, anime4kChainId_, this);
    }
    compare_->show();
    compare_->raise();
    compare_->activateWindow();
}

void PlayerDialog::copyCurrentFrame() {
    if (closed_ || surface_ == nullptr || !surface_->hasVideo()) {
        flashStatus(tr("No frame to copy"));
        return;
    }
    // grabFramebuffer reads the GL front buffer, so this is the frame on
    // screen at full source resolution - not a scaled grab of the widget,
    // which would hand over a thumbnail on a HiDPI display.
    QImage frame = surface_->grabFramebuffer();
    if (frame.isNull()) {
        flashStatus(tr("Could not read the current frame"));
        return;
    }
    if (!QGuiApplication::clipboard()) {
        flashStatus(tr("Clipboard is unavailable"));
        return;
    }
    QGuiApplication::clipboard()->setImage(frame);
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
    openStream(path, 0, /*resume=*/false);
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
    // The only writer: the strip follows the playlist, never the shell's
    // idle chrome, so the check above about who owns what holds by
    // construction instead of by convention.
    strip_->setVisible(!playlist_.isEmpty());
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
            flashStatus(tr("Episode: %1").arg(label));
    for (const QualityEntry& entry : qualities_) {
        if (entry.height == target) {
            // A new episode starts at its beginning, always. openStream
            // carries the position and the pause across by design, which is
            // right for a quality or a dub swap inside one episode and wrong
            // here: stepping or picking the next one and landing at the same
            // offset into it is not what either gesture means.
            openStream(entry.url, target, /*resume=*/false);
            shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(target));
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
            flashStatus(tr("Track: %1").arg(label));
    for (const QualityEntry& entry : qualities_) {
        if (entry.height == target) {
            openStream(entry.url, target);
            shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(target));
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
            flashStatus(tr("Stream link has expired — refreshing"));
    for (const QualityEntry& entry : qualities_) {
        if (entry.height == target) {
            // A refreshed link is a fresh stream, not a continuation: the old
            // resume point belongs to the expired url.
            openStream(entry.url, target, /*resume=*/false);
            shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(target));
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
    if (subtitlesPopup_->isVisible()) {
        subtitlesPopup_->setEntries(trackEntries(QStringLiteral("sub")));
    }
}

void PlayerDialog::onTrackPicked(const QVariant& payload) {
    if (closed_ || directMedia_ || payload.userType() != QMetaType::QVariantMap) {
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
    if (closed_ || payload.userType() != QMetaType::Int) {
        return;
    }
    const int height = payload.toInt();
    for (const QualityEntry& entry : qualities_) {
        if (entry.height != height) {
            continue;
        }
        flashStatus(tr("Switching to %1p…").arg(height));
        openStream(entry.url, height);
        shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(height));
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
    }
    updateSeriesToggle();
    seriesDrawer_->setVisible(seriesExpanded_);
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
    // one that already retired stays retired. A visible retry chip is the
    // other thing that must survive: it is the only offer a failed direct
    // stream makes, and an auto-hidden panel underneath it reads as a dead
    // window with no way back.
    if (closed_) {
        return;
    }
    if (errorActive_ || retryButton_->isVisible()) {
        statusPanel_->show();
    } else if (statusTimer_->isActive()) {
        statusPanel_->show();
        statusTimer_->start(kStatusMs);
    }
}

void PlayerDialog::flashStatus(const QString& message) {
    if (closed_) {
        return;
    }
    errorActive_ = false;
    retryButton_->hide();
    statusLabel_->setText(message);
    statusTimer_->start(kStatusMs);
    statusPanel_->show();
    statusPanel_->raise();
}

void PlayerDialog::showErrorStatus(const QString& message) {
    if (closed_) {
        return;
    }
    errorActive_ = true;
    statusTimer_->stop();
    statusLabel_->setText(message);
    retryButton_->show();
    statusPanel_->show();
    statusPanel_->raise();
}

void PlayerDialog::showError(const QString& message) { onEngineError(message); }

void PlayerDialog::showProcessingStatus(const QString& reason) {
    if (closed_) {
        return;
    }
    // The media path's chip is deliberately not reused: a processing failure
    // has no stream to retry, so the retry chip stays down and the host is never
    // asked to re-extract. The message is the fact and nothing else.
    errorActive_ = false;
    retryButton_->hide();
    statusLabel_->setText(reason);
    statusTimer_->start(kStatusMs);
    statusPanel_->show();
    statusPanel_->raise();
}

void PlayerDialog::retryPlayback() {
    if (closed_ || url_.isEmpty()) {
        return;
    }
    if (!engineReady_) {
        // There is no engine to retry with: the host has to provision the mpv
        // runtime first, and startPlayer() is what runs again afterwards.
        emit runtimeRequested();
        return;
    }
    // The retry starts the stream over, so no resume point survives it.
    openStream(url_, currentHeight_, /*resume=*/false);
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
        showErrorStatus(tr("Could not play the stream"));
        if (!directMedia_ && !refreshAttempted_) {
            refreshAttempted_ = true;
            host_->playbackFailed();
        }
        emit runtimeRequested();
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
    const QSize hint = statusPanel_->sizeHint();
    statusPanel_->setGeometry(area.x() + (area.width() - hint.width()) / 2, area.y() + kStripMargin,
                              hint.width(), hint.height());
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
            if (fullscreen_) {
                return false;
            }
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
    closed_ = true;
    statusTimer_->stop();
    audioPopup_->close();
    subtitlesPopup_->close();
    leavePip(/*closing=*/true);
    shell_->close();
    // VideoSurface owns the libmpv handle: a dialog closed by the host's
    // shutdown must not leave the core running behind it.
    surface_->close();
    if (host_ != nullptr) {
        host_->playerClosed();
    }
    QDialog::closeEvent(event);
}

// ═══════════════════════════════════════════════════════════════════════════
//  picture in picture
// ═══════════════════════════════════════════════════════════════════════════

bool PlayerDialog::isPipActive() const { return pip_ != nullptr; }

void PlayerDialog::togglePip() {
    // Float the video in a borderless always-on-top window. The float decodes
    // the same url in its own surface: mpv gives one core per surface, and
    // sharing one would mean reworking VideoSurface's handle ownership.
    if (closed_) {
        return;
    }
    if (pip_ != nullptr) {
        leavePip();
    } else {
        enterPip();
    }
}

void PlayerDialog::enterPip() {
    if (url_.isEmpty()) {
        return;
    }
    pipWasFullscreen_ = fullscreen_;
    if (fullscreen_) {
        fullscreen_ = false;
        transitioning_ = true;
        if (isFullScreen()) {
            showNormal();
        }
        transitioning_ = false;
        shell_->leaveFullscreen();
    }
    // Parentless on purpose: a child window is clipped to its parent's z-order,
    // so a float parented to the player slides under whatever app the user
    // switches to.
    // Reused across entries. Building or destroying a second QOpenGLWidget
    // while the player's is on screen faults the display driver in
    // DrvPresentBuffers, so the float outlives a single PiP session.
    const bool fresh = pipPool_ == nullptr;
    auto* window = fresh ? new PipFloat : pipPool_;
    pipPool_ = window;
    // Connected once. The float outlives a single PiP session, and connecting
    // again on every entry would stack a second copy of each handler, so one
    // restore click would run leavePip repeatedly.
    if (fresh) {
        // No play/seek signals: the float drives its own core, so routing them
        // to the main surface would move a video the viewer cannot see.
        connect(window, &PipFloat::muteChanged, this, [this](bool muted) {
            muted_ = muted;
            surface_->setMuted(muted_);
            shell_->setMuted(muted_);
        });
        // Two buttons, two meanings: the expand icon returns to the full
        // player, the X closes it for good. Wiring both to leavePip made the X
        // put the full window back, which is the opposite of what it promises.
        connect(window, &PipFloat::restoreRequested, this, [this] { leavePip(); });
        // close() alone is enough: the dialog's close event is what parks and
        // pauses the float, and it reads pip_, not the local `window`, so a
        // pooled float is still the one that gets put away.
        connect(window, &PipFloat::closeRequested, this, &PlayerDialog::close);
    }
    // The float owns a second core, so it needs the chain separately: the mode
    // alone would drop a chain the compare window applied. It is pooled, so the
    // recorded id is compared rather than assumed - "off" clears whatever a
    // previous session left on it.
    window->videoSurface()->setAnime4KMode(anime4kMode_);
    const QString floatChain =
        anime4kChainId_.isEmpty() ? anime4k::modeId(anime4kMode_) : anime4kChainId_;
    if (!floatChain.isEmpty() && window->videoSurface()->anime4KChainId() != floatChain) {
        window->videoSurface()->setAnime4KChain(floatChain);
    }

    window->setPlaying(surface_->isPlaying());
    window->setTime(surface_->position(), surface_->duration());
    window->setMuted(muted_);
    window->openStream(url_, surface_->position());
    // Volume lives on the core and the float has its own. Without this it
    // opens at mpv's default 100% while the player sits wherever the viewer
    // set it, so the volume jumps on entering PiP.
    window->videoSurface()->setVolume(volume_ * 100.0);
    // The float has its own mpv core, so the main one keeps decoding behind a
    // hidden window unless it is stopped. That doubled the bandwidth, burned
    // the rate budget, and meant coming back to two videos at different
    // positions. Only the float plays while PiP is up.
    pipResumePosition_ = surface_->position();
    pipResumePlaying_ = surface_->isPlaying();
    surface_->setPaused(true);

    window->resize(kPipDefaultWidth, kPipDefaultHeight);
    // Placed against the screen under the pointer, not against the player.
    // The whole point of a float is to move it to another monitor, and
    // opening it in the corner of the fullscreen player put it exactly where
    // the viewer was already looking.
    if (QScreen* current = QGuiApplication::screenAt(QCursor::pos())) {
        window->moveToCorner(current);
    } else if (QScreen* fallback = screen()) {
        window->moveToCorner(fallback);
    }
    pip_ = window;
    window->show();
    window->raise();
    syncPipBar();
            flashStatus(tr("Picture in picture"));
    // The float owns the picture now, so what is left behind would be an empty
    // black frame.
    hide();
}

void PlayerDialog::releasePipPool() {
    PipFloat* pool = pipPool_;
    pipPool_ = nullptr;
    pip_ = nullptr;
    if (pool == nullptr) {
        return;
    }
    // NVIDIA faults if the pooled GL widget dies while the main surface is visible.
    hide();
    pool->hide();
    pool->videoSurface()->setPaused(true);
    pool->closeStream();
    delete pool;
}

void PlayerDialog::leavePip(bool closing) {
    PipFloat* window = pip_;
    pip_ = nullptr;
    // Everything the viewer did in PiP happened in the float's core, so that
    // is where the main player resumes - not the position it was parked at on
    // entry. Read before anything else, and NOT clamped against the main
    // surface's duration: the two cores can disagree, and clamping a good
    // position to zero would silently skip the hand-back.
    const double resumeAt = window != nullptr ? std::max(0.0, window->position()) : 0.0;
    if (window != nullptr) {
        // Hidden, not destroyed. Tearing down its QOpenGLWidget while the
        // player's is coming back crashed the display driver; keeping the float
        // alive costs one idle surface and makes the next entry instant. It is
        // released with the dialog.
        window->hide();
        // Paused, though: a hidden float still has a live mpv core, and a
        // playing one decodes the stream a second time behind the player.
        window->videoSurface()->setPaused(true);
    }
    if (closed_) {
        return;
    }
    const bool wasFullscreen = pipWasFullscreen_;
    pipWasFullscreen_ = false;
    if (closing) {
        return;
    }
    QTimer::singleShot(0, this, [this, resumeAt, wasFullscreen] {
        if (closed_) {
            return;
        }
        if (!isVisible()) {
            show();
        }
        if (wasFullscreen && !fullscreen_) {
            fullscreen_ = true;
            transitioning_ = true;
            showFullScreen();
            transitioning_ = false;
            shell_->enterFullscreen();
        }
        if (surface_->hasVideo()) {
            if (resumeAt > 0.0) {
                surface_->seek(resumeAt);
                shell_->setPosition(resumeAt);
            }
            surface_->setPaused(!pipResumePlaying_);
        }
    });
            flashStatus(tr("Returned to window"));
}

void PlayerDialog::syncPipBar() {
    if (pip_ == nullptr) {
        return;
    }

    // Volume and mute carry across: the viewer expects them to, and the float
    // is a second core that knows nothing about them. Position and play state
    // are deliberately not pushed - the main core is stopped while PiP is up,
    // and feeding its frozen state back is what pinned the counter to the
    // moment PiP opened.
    pip_->setMuted(muted_);
    pip_->setVolume(volume_);
}

}  // namespace shell
}  // namespace mplayer
