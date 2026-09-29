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

#include "PlayerDialog.hpp"

#include <QApplication>

#include "PlayerPopups.hpp"
#include "PlayerShell.hpp"
#include "ShellTheme.hpp"
#include "VideoSurface.hpp"

#include <QCoreApplication>
#include <QEasingCurve>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QRegion>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <functional>

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

constexpr int kSeriesWidth = 190;
constexpr int kStripHeight = 30;
constexpr int kStatusMs = 1900;
constexpr int kStatusLabelWidth = 560;
constexpr int kStripMargin = 8;

constexpr int kPipSeekStepSeconds = 10;
constexpr int kPipCornerRadiusPx = 16;
constexpr int kPipBarFadeMs = 160;
constexpr int kPipBarRevealHoldMs = 1600;
constexpr int kPipDragThresholdPx = 6;
constexpr int kPipCenterButtonSize = 84;
constexpr int kPipCenterIconSize = 34;
constexpr int kPipFieldHeight = 32;
constexpr int kPipPillHeight = kPipFieldHeight + 2 * kSpacingXS + 2;
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
               "QWidget#pipPill, QWidget#pipBar {"
               "  background: rgba(24, 25, 28, 200);"
               "  border: 1px solid rgba(231, 231, 234, 0.10);"
               "  border-radius: 16px;"
               "}"
               "QLabel#pipElapsed, QLabel#pipRemaining {"
               "  background: transparent; color: %1; font-size: 12px; font-family: monospace;"
               "}"
               "QPushButton#pipPlay {"
               "  background: rgba(24, 25, 28, 200);"
               "  border: 1px solid rgba(231, 231, 234, 0.10);"
               "  border-radius: %2px; padding: 0;"
               "  min-width: %3px; max-width: %3px;"
               "  min-height: %3px; max-height: %3px;"
               "}"
               "QPushButton#pipMute, QPushButton#pipRestore, QPushButton#pipClose {"
               "  background: transparent; border: 1px solid transparent;"
               "  border-radius: 8px; padding: 0;"
               "}"
               "QPushButton#pipPlay:hover, QPushButton#pipMute:hover,"
               "QPushButton#pipRestore:hover, QPushButton#pipClose:hover {"
               "  background: %4;"
               "}"
               "QSlider#pipScrub { background: transparent; }"
               "QSlider#pipScrub::groove:horizontal {"
               "  height: %5px; background: %6; border-radius: 2px; }"
               "QSlider#pipScrub::sub-page:horizontal { background: %1; border-radius: 2px; }"
               "QSlider#pipScrub::handle:horizontal {"
               "  background: %7; width: %8px; height: %8px;"
               "  margin: %9px 0; border-radius: %10px; }")
        .arg(kMuted)
        .arg(kPipCenterButtonSize / 2)
        .arg(kPipCenterButtonSize)
        .arg(kGhostHover)
        .arg(kPipTimelineHeight)
        .arg(kSurfaceActive)
        .arg(kText)
        .arg(kPipTimelineHandle)
        .arg(kPipTimelineHeight / 2 - kPipTimelineHandle / 2)
        .arg(kPipTimelineHandle / 2);
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
}

void PipFloat::buildChrome() {
    overlay_ = new QWidget(this);
    overlay_->setObjectName(QStringLiteral("pipOverlay"));
    overlay_->setAttribute(Qt::WA_TranslucentBackground);
    overlay_->setFocusPolicy(Qt::StrongFocus);
    overlay_->setMouseTracking(true);
    overlay_->setStyleSheet(pipQss());
    overlay_->installEventFilter(this);
    // The overlay is full-rect and would otherwise eat the gesture; the filter
    // above forwards its presses here so click-to-play and drag-to-move land in
    // one place.
    overlay_->setFocusProxy(this);

    auto* layout = new QVBoxLayout(overlay_);
    layout->setContentsMargins(kSpacingSmall, kSpacingSmall, kSpacingSmall, kSpacingSmall);
    layout->setSpacing(0);

    topPill_ = buildPill(QStringLiteral("pipPill"));
    auto* topRow = new QHBoxLayout(topPill_);
    topRow->setContentsMargins(kSpacingSmall, kSpacingXS, kSpacingSmall, kSpacingXS);
    topRow->setSpacing(kSpacingSmall);

    muteButton_ = buildButton(QStringLiteral("pipMute"), QStringLiteral("Вимкнути звук"),
                              kPipFieldHeight);
    connect(muteButton_, &QPushButton::clicked, this, [this] {
        setMuted(!muted_);
        revealControls();
        emit muteChanged(muted_);
    });
    topRow->addWidget(muteButton_);

    topRow->addStretch(1);

    restoreButton_ =
        buildButton(QStringLiteral("pipRestore"), QStringLiteral("Повернути у вікно"),
                    kPipFieldHeight);
    connect(restoreButton_, &QPushButton::clicked, this, &PipFloat::restoreRequested);
    topRow->addWidget(restoreButton_);

    closeButton_ = buildButton(QStringLiteral("pipClose"), QStringLiteral("Закрити (Esc)"),
                               kPipFieldHeight);
    connect(closeButton_, &QPushButton::clicked, this, &PipFloat::closeRequested);
    topRow->addWidget(closeButton_);

    // The transport control is the one thing a float exists for, so it is the
    // largest mark on the picture and it carries a white triangle rather than
    // sitting in a chip the size of the others.
    playButton_ = new QPushButton(overlay_);
    playButton_->setObjectName(QStringLiteral("pipPlay"));
    playButton_->setFixedSize(kPipCenterButtonSize, kPipCenterButtonSize);
    playButton_->setIcon(glyphIcon(&IconPainter::pause, kPipCenterIconSize));
    playButton_->setToolTip(QStringLiteral("Пауза (Пробіл)"));
    playButton_->setAccessibleName(QStringLiteral("Пауза"));
    playButton_->setFocusPolicy(Qt::NoFocus);
    connect(playButton_, &QPushButton::clicked, this, &PipFloat::togglePlayRequested);

    bar_ = buildPill(QStringLiteral("pipBar"));
    auto* bottomRow = new QHBoxLayout(bar_);
    bottomRow->setContentsMargins(kSpacingSmall, kSpacingXS, kSpacingSmall, kSpacingXS);
    bottomRow->setSpacing(kSpacingSmall);

    elapsed_ = new QLabel(QStringLiteral("0:00"), bar_);
    elapsed_->setObjectName(QStringLiteral("pipElapsed"));
    bottomRow->addWidget(elapsed_);

    scrub_ = new QSlider(Qt::Horizontal, bar_);
    scrub_->setObjectName(QStringLiteral("pipScrub"));
    scrub_->setFocusPolicy(Qt::NoFocus);
    scrub_->setRange(0, 1);
    connect(scrub_, &QSlider::sliderPressed, this, [this] {
        scrubbing_ = true;
        revealControls();
    });
    connect(scrub_, &QSlider::sliderReleased, this, [this] {
        scrubbing_ = false;
        revealControls();
        emit seekAbsoluteRequested(scrub_->value() / 1000.0);
    });
    bottomRow->addWidget(scrub_, 1);

    remaining_ = new QLabel(QStringLiteral("--:--"), bar_);
    remaining_->setObjectName(QStringLiteral("pipRemaining"));
    bottomRow->addWidget(remaining_);

    // The overlay is full rect, so the two pills and the centre button stack
    // over the picture instead of taking a row of it.
    layout->addWidget(topPill_, 0, Qt::AlignTop);
    layout->addStretch(1);
    layout->addWidget(playButton_, 0, Qt::AlignCenter);
    layout->addStretch(1);
    layout->addWidget(bar_, 0, Qt::AlignBottom);

    installEventFilter(this);
    surface_->installEventFilter(this);
    const QVector<QWidget*> watched = {topPill_,  bar_,       elapsed_, remaining_, scrub_,
                                       playButton_, muteButton_, restoreButton_,
                                       closeButton_};
    for (QWidget* widget : watched) {
        widget->installEventFilter(this);
    }
}

QWidget* PipFloat::buildPill(const QString& name) {
    auto* pill = new QWidget(overlay_);
    pill->setObjectName(name);
    pill->setFixedHeight(kPipPillHeight);
    pill->setMouseTracking(true);
    return pill;
}

QPushButton* PipFloat::buildButton(const QString& name, const QString& hint, qreal size) {
    auto* button = new QPushButton(this);
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
    const QString hint = playing ? QStringLiteral("Пауза (Пробіл)")
                                 : QStringLiteral("Продовжити (Пробіл)");
    playButton_->setToolTip(hint);
    playButton_->setAccessibleName(hint);
}

void PipFloat::setTime(double position, double duration) {
    // Display only: nothing is emitted, so answering with a seek would loop.
    if (duration > 0.0) {
        elapsed_->setText(pipFormatSeconds(position));
        remaining_->setText(QStringLiteral("-%1").arg(pipFormatSeconds(std::max(0.0, duration - position))));
    } else {
        // A live stream has no end to count down to, and a slider over an empty
        // range would report every set as zero.
        elapsed_->setText(QStringLiteral("--:--"));
        remaining_->setText(QStringLiteral("--:--"));
    }
    scrub_->setRange(0, std::max(1, static_cast<int>(std::lround(duration * 1000.0))));
    if (!scrubbing_) {
        const double clamped = duration > 0.0 ? std::clamp(position, 0.0, duration) : 0.0;
        scrub_->setValue(static_cast<int>(std::lround(clamped * 1000.0)));
    }
}

void PipFloat::setMuted(bool muted) {
    muted_ = muted;
    const QString hint = muted ? QStringLiteral("Увімкнути звук") : QStringLiteral("Вимкнути звук");
    muteButton_->setIcon(glyphIcon(muted ? &IconPainter::mute : &IconPainter::volume, kIconSize));
    muteButton_->setToolTip(hint);
    muteButton_->setAccessibleName(hint);
}

// ── the controls come and go with the pointer ───────────────────────────

void PipFloat::setOverlayOpacity(double value) {
    overlayOpacity_ = value;
    overlayEffect_->setOpacity(overlayOpacity_);
}

void PipFloat::revealControls() {
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

void PipFloat::pointerRelease(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const bool dragged = dragging_;
    pressed_ = false;
    dragging_ = false;
    if (!dragged) {
        emit togglePlayRequested();
    }
}

// ── layout ───────────────────────────────────────────────────────────────

void PipFloat::fitSurfaces() {
    // The video and the overlay both claim the whole window. Nothing is
    // reserved for chrome: a row left empty at the bottom would be a row of
    // black the user watches instead of the picture.
    surface_->setGeometry(rect());
    overlay_->setGeometry(rect());
    overlay_->raise();
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
            emit seekRequested(-kPipSeekStepSeconds);
            break;
        case Qt::Key_Right:
            emit seekRequested(kPipSeekStepSeconds);
            break;
        case Qt::Key_Space:
            emit togglePlayRequested();
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

PlayerDialog::PlayerDialog(const QString& caption,
                           const QString& url,
                           const QList<QualityEntry>& qualities,
                           int initialHeight,
                           PlayerHost* host,
                           QWidget* parent)
    : QDialog(parent),
      host_(host),
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
      fullscreen_(false),
      seriesExpanded_(false),
      errorActive_(false),
      refreshAttempted_(false),
      closed_(false),
      forwardingKey_(false),
      pipWasFullscreen_(false) {
    setObjectName(QStringLiteral("playerWindow"));
    setWindowTitle(QStringLiteral("Плеєр — ") + caption_);
    setWindowFlag(Qt::FramelessWindowHint, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setMinimumSize(kPlayerMinWidth, kPlayerMinHeight);

    if (QScreen* screen = QGuiApplication::primaryScreen()) {
        resize(screen->availableGeometry().size());
    } else {
        resize(kPlayerDefaultWidth, kPlayerDefaultHeight);
    }
    setContentsMargins(0, 0, 0, 0);

    shell_ = new PlayerShell(this, false);
    shell_->setFocusPolicy(Qt::StrongFocus);
    surface_ = shell_->videoSurface();

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
    audioPopup_ = new MenuPopup(QStringLiteral("ОЗВУЧЕННЯ"), this);
    audioPopup_->setObjectName(QStringLiteral("playerAudioPopup"));
    subtitlesPopup_ = new MenuPopup(QStringLiteral("СУБТИТРИ"), this);
    subtitlesPopup_->setObjectName(QStringLiteral("playerSubtitlesPopup"));

    connect(audioPopup_, &MenuPopup::picked, this, &PlayerDialog::onTrackPicked);
    connect(subtitlesPopup_, &MenuPopup::picked, this, &PlayerDialog::onTrackPicked);
    connect(seriesDrawer_, &SeriesDrawer::picked, this,
            [this](int spot) { goToEpisode(spot, true); });

    watchPanels();
    connectShell();
    connectEngine();

    // A child widget is shown when its parent is, so the panels that start
    // closed have to say so: an empty episode drawer and a retry chip would
    // otherwise appear over the picture on the first paint.
    seriesDrawer_->hide();
    retryButton_->hide();
    publishQualityOptions();
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
    delete pip_;
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
    seriesButton_->setToolTip(QStringLiteral("Список серій (S)"));
    seriesButton_->setAccessibleName(QStringLiteral("Список серій"));
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
        makeChip(statusPanel_, QStringLiteral("playerRetryButton"), QStringLiteral("Повторити"));
    retryButton_->setToolTip(QStringLiteral("Повторити відтворення"));
    retryButton_->setAccessibleName(QStringLiteral("Повторити відтворення"));
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
    connect(shell_, &PlayerShell::openOriginalRequested, this,
            [this] { host_->requestOpenOriginal(); });
    connect(shell_, &PlayerShell::downloadRequested, this, [this] {
        // There is no file behind an HLS stream, so "download" means the link
        // the user can actually save from.
        host_->requestCopyStreamUrl();
        flashStatus(QStringLiteral("Посилання на потік скопійовано"));
    });
    connect(shell_, &PlayerShell::mediaDropped, this, &PlayerDialog::onMediaDropped);
    connect(shell_, &PlayerShell::closeRequested, this, &PlayerDialog::close);
    connect(shell_, &PlayerShell::stateChanged, this, &PlayerDialog::onShellStateChanged);
    connect(shell_, &PlayerShell::audioRequested, this, &PlayerDialog::openAudioMenu);
    connect(shell_, &PlayerShell::subtitlesRequested, this, &PlayerDialog::openSubtitlesMenu);
    connect(shell_, &PlayerShell::qualityRequested, this, &PlayerDialog::onQualityPicked);
    connect(shell_, &PlayerShell::fullscreenChanged, this,
            &PlayerDialog::onShellFullscreenChanged);
    shell_->installEventFilter(this);
}

void PlayerDialog::connectEngine() {
    connect(surface_, &VideoSurface::errorOccurred, this, &PlayerDialog::onEngineError);
    connect(surface_, &VideoSurface::durationChanged, this, &PlayerDialog::onEngineDuration);
    connect(surface_, &VideoSurface::positionChanged, this, &PlayerDialog::onEnginePosition);
    connect(surface_, &VideoSurface::playingChanged, this, &PlayerDialog::onEnginePlaying);
    connect(surface_, &VideoSurface::videoSizeChanged, this, &PlayerDialog::onEngineVideoSize);
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
    showErrorStatus(message.isEmpty() ? QStringLiteral("Не вдалося відтворити потік") : message);
    // One shot: the host re-extracts and reloads, and a second failure must not
    // start the loop over again.
    if (!refreshAttempted_) {
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
    shell_->setAspectRatio(aspect_);
}

bool PlayerDialog::openStream(const QString& url, const QString& quality, bool resume) {
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
    if (!quality.isEmpty()) {
        currentHeight_ = quality.toInt();
        shell_->setQuality(quality);
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
    const bool fullscreen = state == QStringLiteral("FULLSCREEN_ACTIVE") ||
                            state == QStringLiteral("FULLSCREEN_AUTO_HIDDEN");
    if (fullscreen != fullscreen_) {
        fullscreen_ = fullscreen;
    }
    // The shell hides ALL its chrome after an idle moment, and these
    // dialog-owned panels ride along: a strip floating over a blank screen is a
    // stray panel.
    if (state == QStringLiteral("FULLSCREEN_AUTO_HIDDEN")) {
        // Popovers close with the chrome, not with fullscreen. Closing them on
        // "fullscreen" looked right and was wrong: the player is always
        // fullscreen, so every idle/hide transition killed a popover that had
        // just been opened, and the selector appeared not to work at all.
        audioPopup_->close();
        subtitlesPopup_->close();
        seriesDrawer_->setVisible(false);
        strip_->setVisible(false);
        statusPanel_->hide();
    } else {
        strip_->setVisible(true);
        seriesDrawer_->setVisible(seriesExpanded_);
        syncStatusVisibility();
    }
    layoutPanels();
}

void PlayerDialog::onShellFullscreenChanged(bool fullscreen) {
    // The player is a full-screen surface occupying the entire physical
    // monitor. Anything attempting to drop it back to windowed mode is
    // prevented here; the video fills the monitor edge to edge.
    if (closed_) {
        return;
    }
    if (!fullscreen) {
        shell_->enterFullscreen();
    }
    if (pip_ != nullptr) {
        return;
    }
    if (!isFullScreen()) {
        showFullScreen();
    }
    shell_->setGeometry(rect());
}

void PlayerDialog::onMediaDropped(const QString& path) {
    if (closed_ || path.isEmpty()) {
        return;
    }
    // The dropped file replaces the episode, so the qualities of the stream
    // just left behind would offer to switch back into what is not playing.
    qualities_.clear();
    currentHeight_ = 0;
    publishQualityOptions();
    shell_->setMedia(path, QStringLiteral("Локальний файл"));
    openStream(path, QString(), /*resume=*/false);
    flashStatus(QStringLiteral("Відтворюю локальний файл"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  episodes
// ═══════════════════════════════════════════════════════════════════════════

void PlayerDialog::setPlaylist(const QVector<QVariantMap>& playlist, int index) {
    playlist_ = playlist;
    episodeIndex_ = (index >= 0 && index < playlist_.size()) ? index : 0;
    seriesButton_->setVisible(!playlist_.isEmpty());

    QStringList labels;
    labels.reserve(playlist_.size());
    for (const QVariantMap& item : playlist_) {
        labels.append(item.value(QStringLiteral("label")).toString());
    }
    seriesDrawer_->setEntries(labels, episodeIndex_);
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
    if (closed_ || position < 0 || position >= playlist_.size()) {
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
    qualities_ = usableQualities(qualities);
    publishQualityOptions();
}

void PlayerDialog::applyEpisode(const QString& label,
                                const QString& embed,
                                int index,
                                const QList<QualityEntry>& qualities,
                                const QVector<QVariantMap>& variants) {
    if (closed_ || qualities.isEmpty()) {
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
    setWindowTitle(QStringLiteral("Плеєр — ") + caption_);
    setPlaylist(playlist_, index);
    flashStatus(QStringLiteral("Серія: %1").arg(label));
    for (const QualityEntry& entry : qualities_) {
        if (entry.height == target) {
            openStream(entry.url, QStringLiteral("%1p").arg(target));
            shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(target));
            return;
        }
    }
}

void PlayerDialog::applyVariant(const QList<QualityEntry>& qualities,
                                const QString& label,
                                const QString& embed) {
    if (closed_ || qualities.isEmpty()) {
        return;
    }
    if (!embed.isEmpty()) {
        currentEmbed_ = embed;
    }
    setQualities(qualities);
    const int target = pickTargetHeight(0);
    flashStatus(QStringLiteral("Доріжка: %1").arg(label));
    for (const QualityEntry& entry : qualities_) {
        if (entry.height == target) {
            openStream(entry.url, QStringLiteral("%1p").arg(target));
            shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(target));
            return;
        }
    }
}

void PlayerDialog::applyRefreshedStreams(const QList<QualityEntry>& qualities, int height) {
    if (closed_ || qualities.isEmpty()) {
        return;
    }
    setQualities(qualities);
    const int target = pickTargetHeight(height);
    flashStatus(QStringLiteral("Посилання на потік прострочене — оновлюю"));
    for (const QualityEntry& entry : qualities_) {
        if (entry.height == target) {
            // A refreshed link is a fresh stream, not a continuation: the old
            // resume point belongs to the expired url.
            openStream(entry.url, QStringLiteral("%1p").arg(target), /*resume=*/false);
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
    if (closed_) {
        return;
    }
    audioPopup_->setEntries(trackEntries(QStringLiteral("voice")));
    if (audioPopup_->rowCount() == 0) {
        flashStatus(QStringLiteral("Немає інших озвучень"));
        return;
    }
    probeUnknownTracks();
    audioPopup_->popupNear(settingsAnchor());
}

void PlayerDialog::openSubtitlesMenu() {
    if (closed_) {
        return;
    }
    subtitlesPopup_->setEntries(trackEntries(QStringLiteral("sub")));
    if (subtitlesPopup_->rowCount() == 0) {
        flashStatus(QStringLiteral("Немає доступних субтитрів"));
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
    if (closed_ || payload.userType() != QMetaType::QVariantMap) {
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
        flashStatus(QStringLiteral("Перемикання на %1p…").arg(height));
        openStream(entry.url, QStringLiteral("%1p").arg(height));
        shell_->setMedia(entry.url, caption_, QStringLiteral("%1p").arg(height));
        return;
    }
}


QPoint PlayerDialog::settingsAnchor() const {
    for (QPushButton* button : shell_->findChildren<QPushButton*>()) {
        if (button->toolTip() == QStringLiteral("toggle_settings_popover")) {
            return button->mapToGlobal(QPoint(button->width(), 0));
        }
    }
    return shell_->mapToGlobal(QPoint(shell_->width() / 2, shell_->height() - 46));
}


void PlayerDialog::toggleSeries() { setSeriesExpanded(!seriesExpanded_); }

void PlayerDialog::setSeriesExpanded(bool expanded) {
    seriesExpanded_ = expanded;
    if (!seriesExpanded_) {
        audioPopup_->close();
        subtitlesPopup_->close();
    }
    updateSeriesToggle();
    seriesDrawer_->setVisible(seriesExpanded_);
    layoutPanels();
}

void PlayerDialog::updateSeriesToggle() {
    const QString hint = seriesExpanded_ ? QStringLiteral("Сховати список серій (S)")
                                         : QStringLiteral("Список серій (S)");
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
    // one that already retired stays retired.
    if (closed_) {
        return;
    }
    if (errorActive_) {
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
    const QString quality = currentHeight_ > 0 ? QStringLiteral("%1p").arg(currentHeight_) : QString();
    // The retry starts the stream over, so no resume point survives it.
    openStream(url_, quality, /*resume=*/false);
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
    if (!openStream(url_, QString(), /*resume=*/false)) {
        // No mpv core: the host has to provision the runtime before this can
        // work, and calling startPlayer() again is what retries it.
        showErrorStatus(QStringLiteral("Не вдалося відтворити потік"));
        if (!refreshAttempted_) {
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
    strip_->move(area.x() + kSpacingMedium, area.y() + kStripMargin);
    const QSize hint = statusPanel_->sizeHint();
    statusPanel_->setGeometry(area.x() + (area.width() - hint.width()) / 2, area.y() + kStripMargin,
                              hint.width(), hint.height());
    if (seriesExpanded_) {
        const int top = area.y() + kStripMargin + kStripHeight + kSpacingXS;
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

void PlayerDialog::fitToCurrentScreen() {
    // The lightbox adapts to the screen it is on, per monitor. Windows frameless
    // windows do not always honour showFullScreen() the first time, and without
    // this explicit pin the player would sit at its constructed default size for
    // the whole session.
    if (closed_) {
        return;
    }
    QScreen* current = screen();
    if (current == nullptr) {
        current = QGuiApplication::primaryScreen();
    }
    if (current != nullptr) {
        setGeometry(current->geometry());
    }
}

void PlayerDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    fitToCurrentScreen();
    if (!isFullScreen()) {
        showFullScreen();
    }
    shell_->setGeometry(rect());
    shell_->show();
    shell_->setFocus(Qt::OtherFocusReason);
    shell_->enterFullscreen();
    layoutPanels();
}

void PlayerDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    shell_->setGeometry(rect());
    layoutPanels();
}

void PlayerDialog::moveEvent(QMoveEvent* event) {
    QDialog::moveEvent(event);
    // A full-screen lightbox follows the monitor the OS moves it to.
    if (!closed_ && isFullScreen()) {
        if (QScreen* current = QWidget::screen()) {
            if (geometry() != current->geometry()) {
                fitToCurrentScreen();
            }
        }
    }
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
        } else if (shell_->dismissOverlay()) {
            // The settings popover and the key hints are Qt::Popup windows: they
            // hold the keyboard while open, so this is the only place the key
            // can reach them.
        } else if (seriesExpanded_) {
            setSeriesExpanded(false);
        } else {
            // No overlay left, so close. Falling through to the shell here meant
            // Esc only cleared the shell's fullscreen flag - invisible, because
            // this dialog refits itself to the monitor - and the player looked
            // unresponsive while the user pressed it again and again.
            close();
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
        // Activating the owner raises it, and a raised owner goes above its own
        // popover. Restoring focus while one is open is what made the pickers
        // vanish behind the picture.
        if (shell_ != nullptr && QApplication::activePopupWidget() == nullptr) {
            shell_->setFocus(Qt::OtherFocusReason);
        }
        return false;
    }
    if (watched == shell_ && event->type() == QEvent::KeyPress) {
        return onShellKey(static_cast<QKeyEvent*>(event));
    }
    // Panel activity counts as user presence: re-arm the shell's idle timer
    // without consuming the event.
    if (!panelWatchers_.isEmpty() && panelWatchers_.contains(watched)) {
        switch (event->type()) {
            case QEvent::Enter:
            case QEvent::HoverEnter:
            case QEvent::HoverMove:
            case QEvent::MouseMove:
            case QEvent::MouseButtonPress:
            case QEvent::Wheel:
                shell_->wake();
                break;
            default:
                break;
        }
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
    host_->playerClosed();
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
    pipWasFullscreen_ = isFullScreen();
    if (pipWasFullscreen_) {
        showNormal();
    }
    // Parentless on purpose: a child window is clipped to its parent's z-order,
    // so a float parented to the player slides under whatever app the user
    // switches to.
    auto* window = new PipFloat;
    connect(window, &PipFloat::togglePlayRequested, surface_, &VideoSurface::togglePause);
    connect(window, &PipFloat::seekRequested, this, [this](double delta) {
        surface_->seek(surface_->position() + delta);
    });
    connect(window, &PipFloat::seekAbsoluteRequested, surface_, &VideoSurface::seek);
    connect(window, &PipFloat::muteChanged, this, [this](bool muted) {
        muted_ = muted;
        surface_->setMuted(muted_);
        shell_->setMuted(muted_);
    });
    connect(window, &PipFloat::restoreRequested, this, [this] { leavePip(); });
    connect(window, &PipFloat::closeRequested, this, [this] { leavePip(); });

    window->setPlaying(surface_->isPlaying());
    window->setTime(surface_->position(), surface_->duration());
    window->setMuted(muted_);
    window->openStream(url_, surface_->position());

    window->resize(kPipDefaultWidth, kPipDefaultHeight);
    if (QScreen* current = screen()) {
        const QRect area = current->availableGeometry();
        window->move(area.right() - kPipDefaultWidth - 24, area.bottom() - kPipDefaultHeight - 24);
    }
    pip_ = window;
    window->show();
    window->raise();
    syncPipBar();
    flashStatus(QStringLiteral("Картинка в картинці"));
    // The float owns the picture now, so what is left behind would be an empty
    // black frame.
    hide();
}

void PlayerDialog::leavePip(bool closing) {
    PipFloat* window = pip_;
    pip_ = nullptr;
    if (window != nullptr) {
        window->closeStream();
        window->close();
        window->deleteLater();
    }
    if (closed_) {
        return;
    }
    if (!isVisible()) {
        show();
    }
    if (pipWasFullscreen_) {
        showFullScreen();
    }
    pipWasFullscreen_ = false;
    if (!closing) {
        flashStatus(QStringLiteral("Повернуто у вікно"));
    }
}

void PlayerDialog::syncPipBar() {
    if (pip_ == nullptr) {
        return;
    }
    pip_->setPlaying(surface_->isPlaying());
    pip_->setTime(surface_->position(), surface_->duration());
    pip_->setMuted(muted_);
}

}  // namespace shell
}  // namespace mplayer
