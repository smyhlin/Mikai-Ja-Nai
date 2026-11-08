// Frameless always-on-top Picture-in-Picture float window and painted chrome.

#include "PipFloat.hpp"

#include "PlayerShell.hpp"
#include "ShellSliders.hpp"
#include "ShellTheme.hpp"
#include "ShellWidgets.hpp"
#include "VideoSurface.hpp"

#include <QCloseEvent>
#include <QCursor>
#include <QEasingCurve>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace mplayer {
namespace shell {

namespace {

constexpr int kSpacingXS = 4;
constexpr int kSpacingSmall = 8;
constexpr int kSpacingMedium = 12;
constexpr int kIconSize = 18;
// T-404: kPipDefaultWidth/Height live in PipFloat.hpp now.

constexpr int kPipSeekStepSeconds = 10;
constexpr int kPipCornerRadiusPx = 16;
// The gap a parked float keeps from the work-area edge: far enough that a
// maximized window does not look like it is clipping it, close enough that
// the controls at the edges are reachable. Only the FIRST placement of a
// brand-new float uses a corner; a viewer-placed float is free and this offset
// never applies to it.
constexpr int kPipBoundsOffset = 24;
constexpr int kPipBarFadeMs = 160;
constexpr int kPipBarRevealHoldMs = 1600;
constexpr int kPipDragThresholdPx = 6;
constexpr int kPipCenterButtonSize = 56;
constexpr int kPipCenterIconSize = 22;
constexpr int kPipFieldHeight = 30;
constexpr int kPipInnerButtonSize = 24;
constexpr int kPipVolumeWidth = 76;
constexpr int kPipTimelineHeight = 3;
constexpr int kPipTimelineHandle = 10;

bool isWayland() {
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"),
                                                       Qt::CaseInsensitive);
}


/// Seconds as m:ss, gaining an hours field only when it is needed.
QString pipFormatSeconds(double value) {
    // A non-finite or negative timestamp (an mpv property spike before the
    // core settles) must not overflow the cast or show a negative clock.
    if (!std::isfinite(value) || value <= 0.0) return QStringLiteral("0:00");
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

}  // namespace


// ═══════════════════════════════════════════════════════════════════════════
//  PipFloat
// ═══════════════════════════════════════════════════════════════════════════

PipFloat::PipFloat(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setObjectName(QStringLiteral("playerPip"));
    // The float is frameless, so this title is never painted: it is the name a
    // window list and assistive technology read, and it is retranslated live.
    setWindowTitle(tr("Picture in picture"));
    setMinimumSize(kPipDefaultWidth / 2, kPipDefaultHeight / 2);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    surface_ = new VideoSurface(this);
    surface_->setObjectName(QStringLiteral("pipVideo"));
    surface_->setFocusPolicy(Qt::NoFocus);
    surface_->setMouseTracking(true);
    // The float is a translucent top level, so the surface's alpha is the
    // window's shape: the corners are coverage on the composited output, not a
    // one-bit window mask, and the desktop shows through them. The setter keeps
    // the raw framebuffer untouched.
    surface_->setDisplayCornerRadius(kPipCornerRadiusPx);
    // T-205: no in-app consumer reads the raw counters; the pooled float must
    // not poll either. The probes keep their own surfaces with polling on.
    surface_->setDiagnosticsPolling(false);

    buildChrome();

    // The chrome is drawn by the picture's own painter, in the same paintGL
    // pass that carried mpv's frame. The controls are adopted here: they keep
    // their hit target, tooltip, accessible name and layout slot, and give up
    // only their pixels. The painter is connected once, directly - every paint
    // of the surface, forced or not, carries them.
    connect(surface_, &VideoSurface::paintOverlay, this, &PipFloat::paintSurfaceOverlay,
            Qt::DirectConnection);
    // Adopted once, here: every control below stops painting itself and is
    // recorded in paint order, so a frame is a walk of that list.
    chrome_.adopt(this, this);

    // The accent and the chrome's cached pixels are both read at paint time, so
    // a token change has to drop the caches, not merely ask for a repaint - a
    // repaint that blits the old images is the stale-pixel case.
    connect(&accentNotifier(), &AccentNotifier::changed, this, [this] { chromeDirtyAll(); });

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
            if (pendingLoadPause_ &&
                surface_->pauseRequestGeneration() == pendingLoadPauseGeneration_) {
                surface_->setPaused(*pendingLoadPause_);
            }
            pendingLoadPause_.reset();
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
    const auto suspendPumpWhenParked = [this] {
        if (!isVisible() && surface_ != nullptr &&
            surface_->nativePlaybackStatus().pendingRequestId.isEmpty() &&
            !surface_->animeJanaiEngineBuildActive()) {
            surface_->setPumpSuspended(true);
        }
    };
    connect(surface_, &VideoSurface::nativePlaybackStatusChanged, this,
            [suspendPumpWhenParked](const native::NativePlaybackController::Status&) {
                suspendPumpWhenParked();
            });
    connect(surface_, &VideoSurface::animeJanaiEngineBuildChanged, this,
            [suspendPumpWhenParked](bool, bool, const QString&) {
                suspendPumpWhenParked();
            });
}

void PipFloat::buildChrome() {
    overlay_ = new QWidget(this);
    overlay_->setObjectName(QStringLiteral("pipOverlay"));
    overlay_->setAttribute(Qt::WA_TranslucentBackground);
    overlay_->setFocusPolicy(Qt::StrongFocus);
    overlay_->setMouseTracking(true);
    overlay_->installEventFilter(this);
    // The overlay is full-rect and forwards input to the float for drag/move
    // and click-to-pause.
    overlay_->setFocusProxy(this);

    const double radius = cornerRadius() > 0.0 ? cornerRadius() : 8.0;
    const double panelRadius = std::min(radius + 2.0, kPipFieldHeight / 2.0);
    const double innerRadius = qMax(3.0, panelRadius - 2.0);

    // Left cluster: volume pill with mute button and slider in one sleek container
    volumeChip_ = new Panel(pill(), panelRadius, w15(), overlay_);
    volumeChip_->setObjectName(QStringLiteral("pipVolumeChip"));
    volumeChip_->setFocusPolicy(Qt::NoFocus);
    volumeChip_->setMouseTracking(true);
    volumeChip_->setFixedHeight(kPipFieldHeight);
    auto* chipLayout = new QHBoxLayout(volumeChip_);
    chipLayout->setContentsMargins(4, 3, 6, 3);
    chipLayout->setSpacing(4);

    const QColor muteHoverFill(255, 255, 255, 36);
    muteButton_ = new IconButton(&IconPainter::volume, tr("Mute audio"), kPipInnerButtonSize, 14,
                                 volumeChip_, muteHoverFill);
    muteButton_->setObjectName(QStringLiteral("pipMute"));
    muteButton_->setMouseTracking(true);
    ButtonChrome muteChrome;
    muteChrome.useThemeRadius = false;
    muteChrome.hoverFill = muteHoverFill;
    muteChrome.radius = innerRadius;
    muteChrome.borderWidth = 0;
    muteButton_->setButtonChrome(muteChrome);
    connect(muteButton_, &QPushButton::clicked, this, [this] {
        setMuted(!muted_);
        revealControls();
        emit muteChanged(muted_);
    });
    chipLayout->addWidget(muteButton_);

    volumeSlider_ = new VolumeTrack(volumeChip_);
    volumeSlider_->setObjectName(QStringLiteral("pipVolume"));
    volumeSlider_->setFocusPolicy(Qt::NoFocus);
    volumeSlider_->setMouseTracking(true);
    volumeSlider_->setFixedSize(kPipVolumeWidth, kPipInnerButtonSize);
    volumeSlider_->setValueSilent(volume_);
    connect(volumeSlider_, &VolumeTrack::pressed, this, [this] {
        draggingVolume_ = true;
        revealControls();
    });
    connect(volumeSlider_, &VolumeTrack::released, this, [this] {
        draggingVolume_ = false;
        setVolume(volumeSlider_->value());
        if (muted_ && volume_ > 0.0) {
            setMuted(false);
            emit muteChanged(false);
        }
        emit volumeChanged(volume_);
        revealControls();
    });
    connect(volumeSlider_, &VolumeTrack::valueChanged, this, [this](double value) {
        if (draggingVolume_) {
            volume_ = std::clamp(value, 0.0, 1.0);
            return;
        }
        setVolume(value);
        if (muted_ && volume_ > 0.0) {
            setMuted(false);
            emit muteChanged(false);
        }
        emit volumeChanged(volume_);
        revealControls();
    });
    chipLayout->addWidget(volumeSlider_);

    // Right cluster: symmetrical window controls pill matching volumeChip_ exactly
    windowControls_ = new Panel(pill(), panelRadius, w15(), overlay_);
    windowControls_->setObjectName(QStringLiteral("pipWindowControls"));
    windowControls_->setFocusPolicy(Qt::NoFocus);
    windowControls_->setMouseTracking(true);
    windowControls_->setFixedHeight(kPipFieldHeight);
    auto* controlsLayout = new QHBoxLayout(windowControls_);
    controlsLayout->setContentsMargins(4, 3, 4, 3);
    controlsLayout->setSpacing(2);

    restoreButton_ = new IconButton(&IconPainter::expand, tr("Return to the window"),
                                    kPipInnerButtonSize, 12, windowControls_);
    restoreButton_->setObjectName(QStringLiteral("pipRestore"));
    restoreButton_->setMouseTracking(true);
    ButtonChrome restoreChrome;
    restoreChrome.useThemeRadius = false;
    restoreChrome.hoverFill = w15();
    restoreChrome.radius = innerRadius;
    restoreChrome.borderWidth = 0;
    restoreButton_->setButtonChrome(restoreChrome);
    connect(restoreButton_, &QPushButton::clicked, this, &PipFloat::restoreRequested);
    controlsLayout->addWidget(restoreButton_);

    closeButton_ = new IconButton(&IconPainter::close, tr("Close (Esc)"),
                                  kPipInnerButtonSize, 12, windowControls_);
    closeButton_->setObjectName(QStringLiteral("pipClose"));
    closeButton_->setMouseTracking(true);
    ButtonChrome closeChrome;
    closeChrome.useThemeRadius = false;
    closeChrome.hoverFill = QColor(220, 53, 69, 215);
    closeChrome.radius = innerRadius;
    closeChrome.borderWidth = 0;
    closeButton_->setButtonChrome(closeChrome);
    connect(closeButton_, &QPushButton::clicked, this, &PipFloat::closeRequested);
    controlsLayout->addWidget(closeButton_);

    playButton_ = new IconButton(&IconPainter::pause, tr("Pause (Space)"),
                                 kPipCenterButtonSize, kPipCenterIconSize, overlay_);
    playButton_->setObjectName(QStringLiteral("pipPlay"));
    playButton_->setMouseTracking(true);
    // The tooltip stays the running hint; the accessible name is the short verb
    // the button is announced by.
    playButton_->setAccessibleName(tr("Pause"));
    // The disc the QSS drew, gradient and all - built here because the gradient
    // ends where the button does.
    ButtonChrome playChrome;
    QLinearGradient playFill(0, 0, 0, kPipCenterButtonSize);
    playFill.setColorAt(0.0, QColor(12, 13, 19, 143));
    playFill.setColorAt(1.0, QColor(12, 13, 19, 189));
    playChrome.fill = QBrush(playFill);
    playChrome.border = QColor(255, 255, 255, 77);
    playChrome.borderWidth = 1.5;
    playChrome.radius = kPipCenterButtonSize / 2.0;
    QLinearGradient playHoverFill(0, 0, 0, kPipCenterButtonSize);
    playHoverFill.setColorAt(0.0, QColor(20, 22, 32, 194));
    playHoverFill.setColorAt(1.0, QColor(20, 22, 32, 224));
    playChrome.hoverFill = QBrush(playHoverFill);
    playChrome.hoverBorder = QColor(255, 255, 255, 140);
    playButton_->setButtonChrome(playChrome);
    connect(playButton_, &QPushButton::clicked, surface_, &VideoSurface::togglePause);

    // The QSS these replace measured the clocks in pixels, so this one is a
    // pixel size too: 11 points is ~15px and would widen the row under them.
    QFont clockFont = uiFont(11, true);
    clockFont.setPixelSize(11);
    elapsed_ = new ChromeLabel(QStringLiteral("0:00"), clockFont, QColor(255, 255, 255), overlay_);
    elapsed_->setObjectName(QStringLiteral("pipElapsed"));
    elapsed_->setMouseTracking(true);

    remaining_ = new ChromeLabel(QStringLiteral("--:--"), clockFont, QColor(255, 255, 255),
                                 overlay_);
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
    // The timeline the picture draws is read off this value, so a value the
    // slider takes on its own - a position tick, a drag, a seek - is a repaint
    // of the picture. The widget itself paints nothing (the filter below
    // swallows it), and it is hidden with the rest of the chrome.
    connect(scrub_, &QSlider::valueChanged, this, [this](int) {
        if (surface_ != nullptr && overlayOpacity_ > 0.01) {
            surface_->update();
        }
    });
    // Click-to-seek. The overlay claims every press before the slider sees it,
    // so a click on the timeline fell through to the picture and only
    // toggled playback. An event filter is the only way in: QSlider handles
    // its own press in a protected override, so there is no signal for it.
    scrub_->installEventFilter(this);

    installEventFilter(this);
    surface_->installEventFilter(this);
    const QVector<QWidget*> watched = {elapsed_,        remaining_,      scrub_,
                                       playButton_,     muteButton_,     restoreButton_,
                                       closeButton_,    volumeChip_,     volumeSlider_,
                                       windowControls_};
    for (QWidget* widget : watched) {
        widget->installEventFilter(this);
    }
    // The overlay is where the presses, the drags and the resize grips land,
    // so it stays above the picture. Raised once here: the layout pass is not
    // the place for it, because that runs on every step of a resize.
    overlay_->raise();
}

// ── the picture's own painter draws the chrome ──────────────────────────

void PipFloat::chromeDirty(QWidget* widget, const QRect& rect) {
    // The float's surface fills the float, so a control's pixels are the
    // picture's pixels.
    if (widget == nullptr || surface_ == nullptr) {
        return;
    }
    // The box the control was last painted into is part of the damage: it is
    // translucent, so Qt does not repaint the area it vacates.
    const QRect vacated = chrome_.invalidate(widget);
    const QRect box(rect.isNull() ? widget->rect() : rect);
    QRect dirty(widget->mapTo(this, box.topLeft()), box.size());
    if (!vacated.isEmpty()) dirty = dirty.united(vacated);
    const QRect region(dirty.intersected(surface_->geometry()));
    // A control off the picture has nothing to draw, and no rect of its own
    // describes the pixels it left behind.
    if (region.isEmpty()) {
        surface_->update();
        return;
    }
    surface_->update(region);
}

void PipFloat::chromeMoved(QWidget* widget) {
    if (widget == nullptr || surface_ == nullptr) return;
    // A translation keeps the control's pixels; only its damage moved.
    QRect dirty(widget->mapTo(this, QPoint(0, 0)), widget->size());
    const QRect vacated = chrome_.damage(widget);
    if (!vacated.isEmpty()) dirty = dirty.united(vacated);
    const QRect region(dirty.intersected(surface_->geometry()));
    if (region.isEmpty()) {
        surface_->update();
        return;
    }
    surface_->update(region);
}

void PipFloat::chromeDirtyAll() {
    chrome_.invalidateAll();
    if (surface_ == nullptr) {
        return;
    }
    surface_->update();
    update();
}

void PipFloat::paintSurfaceOverlay(QPainter* painter) {
    if (painter == nullptr || overlayOpacity_ <= 0.01) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    // ChromeList decides the filter for its own cache blits (aligned copies are
    // copied unfiltered); this hint covers everything else the overlay draws
    // from a pixmap at a non-unit scale.
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    // The fade is the painter's: one alpha over the chrome, where an opacity
    // effect re-composited an offscreen buffer of the overlay on every change.
    // The cached images are composited under that alpha, not baked into it.
    painter->setOpacity(overlayOpacity_);
    chrome_.paint(*painter, this, rect());
    // The timeline is not a widget - the scrub slider under it is an input
    // target with no pixels - so it is drawn in the same pass.
    paintTimeline(*painter);
    painter->restore();
}

void PipFloat::paintTimeline(QPainter& painter) {
    if (scrub_ == nullptr || !scrub_->isVisible() || scrub_->width() <= 0) {
        return;
    }
    // The slider's geometry is the timeline's: full-rect overlay child, so its
    // rect is in the float's coordinates and its width is what the press maths
    // divides by.
    const QRect groove = scrub_->geometry();

    // The surface's framebuffer is single-sample (no format in the app asks for
    // samples), so QPainter::Antialiasing on the GL painter has no coverage to
    // resolve: the rounded groove and the handle were aliased there. They are
    // rasterised with the raster engine instead, into a buffer that is reused,
    // and copied back one texel per device pixel.
    const QRect bounds = groove.adjusted(-kPipTimelineHandle / 2, -kPipTimelineHandle / 2,
                                         kPipTimelineHandle / 2, kPipTimelineHandle / 2);
    const qreal ratio =
        painter.device() != nullptr ? painter.device()->devicePixelRatio() : qreal(1.0);
    const QSize deviceSize(qMax(1, static_cast<int>(std::ceil(bounds.width() * ratio))),
                           qMax(1, static_cast<int>(std::ceil(bounds.height() * ratio))));
    const int value = scrub_->value();
    const int maximum = scrub_->maximum();
    const QRgb accentRgba = accent().rgba();

    const bool reusable = timelineCacheValid_ && !timelineCache_.isNull() &&
                          timelineCacheBounds_ == bounds &&
                          timelineCachePixels_ == deviceSize &&
                          timelineCacheValue_ == value && timelineCacheMaximum_ == maximum &&
                          timelineCacheAccent_ == accentRgba &&
                          qFuzzyCompare(timelineCacheRatio_, ratio);
    if (!reusable) {
        if (timelineCache_.isNull() || timelineCache_.size() != deviceSize) {
            timelineCache_ = QPixmap(deviceSize);
        }
        timelineCache_.setDevicePixelRatio(ratio);
        timelineCache_.fill(Qt::transparent);
        QPainter cache(&timelineCache_);
        cache.setRenderHint(QPainter::Antialiasing, true);
        cache.setRenderHint(QPainter::TextAntialiasing, true);
        cache.translate(-bounds.topLeft());

        const double trackY = groove.y() + (groove.height() - kPipTimelineHeight) / 2.0;
        paintRounded(cache, QRectF(groove.x(), trackY, groove.width(), kPipTimelineHeight),
                     kPipTimelineHeight / 2.0, w25());

        // Inset by half a handle at each end - the expression timelineSeekTo()
        // reads a pointer from - so the drawn handle and the seek agree.
        const double span =
            std::max(1.0, static_cast<double>(groove.width()) - kPipTimelineHandle);
        const double at = maximum > 0
            ? std::clamp(static_cast<double>(value) / maximum, 0.0, 1.0)
            : 0.0;
        if (at > 0.0) {
            // The played part, up to the handle: what the slider's sub-page was.
            paintRounded(cache,
                         QRectF(groove.x(), trackY, kPipTimelineHandle / 2.0 + at * span,
                                kPipTimelineHeight),
                         kPipTimelineHeight / 2.0, timelinePlayed());
        }

        const double centerX = groove.x() + kPipTimelineHandle / 2.0 + at * span;
        const double centerY = groove.y() + groove.height() / 2.0;
        cache.setPen(Qt::NoPen);
        cache.setBrush(white(255));
        cache.drawEllipse(QRectF(centerX - kPipTimelineHandle / 2.0,
                                 centerY - kPipTimelineHandle / 2.0, kPipTimelineHandle,
                                 kPipTimelineHandle));

        timelineCacheBounds_ = bounds;
        timelineCachePixels_ = deviceSize;
        timelineCacheRatio_ = ratio;
        timelineCacheValue_ = value;
        timelineCacheMaximum_ = maximum;
        timelineCacheAccent_ = accentRgba;
        timelineCacheValid_ = true;
    }

    const QTransform toDevice = painter.deviceTransform();
    const bool oneToOne = deviceAlignedTransform(toDevice, ratio);
    const QPointF origin = oneToOne ? deviceAlignedOrigin(toDevice, QPointF(bounds.topLeft()))
                                    : QPointF(bounds.topLeft());
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, !oneToOne);
    painter.drawPixmap(origin, timelineCache_);
    painter.restore();
}

void PipFloat::openStream(const QString& url, double position, bool paused, quint64 mediaToken) {
    if (url.isEmpty()) {
        return;
    }
    pendingSeek_ = position;
    surface_->open(url, mediaToken);
    surface_->setPaused(paused);
    pendingLoadPause_ = paused;
    pendingLoadPauseGeneration_ = surface_->pauseRequestGeneration();
    fitSurfaces();
}

void PipFloat::closeStream() {
    pendingSeek_ = -1.0;
    pendingLoadPause_.reset();
    surface_->close();
}

void PipFloat::setPlaying(bool playing) {
    playing_ = playing;
    // The two transport triangles are painted by the button itself, so the
    // glyph function is the only handle on which one is up - a pixmap could not
    // tell them apart at dock scale, and there is none to ask.
    playButton_->setIcon(playing ? &IconPainter::pause : &IconPainter::play);
    const QString hint = playing ? tr("Pause (Space)")
                                 : tr("Play (Space)");
    playButton_->setToolTip(hint);
    playButton_->setAccessibleName(hint);
}

void PipFloat::setTime(double position, double duration) {
    // Retranslation reuses the observed clock without reading the playback core.
    lastPosition_ = position;
    lastDuration_ = duration;
    const QString elapsedText = pipFormatSeconds(position);
    const QString remainingText = duration > 0.0
        ? QStringLiteral("-%1").arg(pipFormatSeconds(std::max(0.0, duration - position)))
        : QStringLiteral("--:--");
    if (elapsed_->text() != elapsedText || remaining_->text() != remainingText) {
        elapsed_->setText(elapsedText);
        remaining_->setText(remainingText);
        elapsed_->adjustSize();
        remaining_->adjustSize();
        remaining_->move(qMax(0, width() - kSpacingMedium - remaining_->width()),
                         remaining_->y());
    }
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
    updateMuteChrome();
    updateVolumeChip();
}

/// The mute button's glyph and wording, from the retained flag. Only the
/// strings are rebuilt here: setMuted() is the one that tells the core.
void PipFloat::updateMuteChrome() {
    const QString hint = muted_ ? tr("Unmute audio") : tr("Mute audio");
    muteButton_->setIcon(muted_ ? &IconPainter::mute : &IconPainter::volume);
    muteButton_->setToolTip(hint);
    muteButton_->setAccessibleName(hint);
}

void PipFloat::setVolume(double volume) {
    volume_ = std::clamp(volume, 0.0, 1.0);
    surface_->setVolume(volume_ * 100.0);
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
        volumeSlider_->setValueSilent(volume_);
    }
}

// ── the controls come and go with the pointer ───────────────────────────

void PipFloat::setOverlayOpacity(double value) {
    overlayOpacity_ = std::clamp(value, 0.0, 1.0);
    // A tick of the fade is a repaint of the picture with a new alpha: the cached
    // chrome pixels are unchanged because the opacity is applied by the painter
    // over their blits. Wiping the cache here re-rasterised every control on every
    // fade tick - the same defect the shell had on every pointer move.
    if (surface_ == nullptr) {
        update();
    } else {
        surface_->update();
    }
    // Faded out means hidden as widgets too, or the controls would still be
    // there to swallow the press that was meant for the picture: the play
    // button under the pointer is what toggles playback.
    overlay_->setVisible(overlayOpacity_ > 0.01);
}

void PipFloat::revealControls() {
    hovered_ = isVisible() && rect().contains(mapFromGlobal(QCursor::pos()));
    // Mouse motion over revealed controls must not repaint the paused video.
    if (hovered_ && overlayOpacity_ >= 1.0 && overlayFade_->state() != QAbstractAnimation::Running
        && hideTimer_->isActive()) {
        return;
    }
    // The reveal is instant on purpose: a fade in makes the controls feel late
    // to a pointer that is already on the picture.
    hideTimer_->stop();
    overlayFade_->stop();
    setOverlayOpacity(1.0);
    // Layered child windows can miss leave events; keep a bounded pointer check armed.
    hideTimer_->start(kPipBarRevealHoldMs);
}

void PipFloat::hideControls() {
    hovered_ = isVisible() && rect().contains(mapFromGlobal(QCursor::pos()));
    if (hovered_) {
        hideTimer_->start(kPipBarRevealHoldMs);
        return;
    }
    if (overlayOpacity_ <= 0.01) return;
    overlayFade_->stop();
    overlayFade_->setStartValue(overlayOpacity_);
    overlayFade_->setEndValue(0.0);
    overlayFade_->start();
}

bool PipFloat::eventFilter(QObject* watched, QEvent* event) {
    // The timeline is painted into the picture, so the slider under it must not
    // paint its own groove and handle over the video. Nothing of its painting is
    // load-bearing: the click-to-seek below claims every press.
    if (watched == scrub_ && event->type() == QEvent::Paint) {
        return true;
    }
    // A control that changed through Qt rather than chromeUpdate() still has to
    // reach the picture. The event is never consumed here.
    chromeRepaintEvent(this, watched, event);
    if (event->type() == QEvent::Wheel && (watched == muteButton_ || watched == volumeChip_)) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        if (wheel->pixelDelta().y() != 0 || wheel->angleDelta().y() != 0) {
            QCoreApplication::sendEvent(volumeSlider_, event);
            revealControls();
            return event->isAccepted();
        }
    }
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

bool PipFloat::event(QEvent* event) {
    // QWidget::event() consumes both of these without forwarding them to
    // changeEvent(), so this override is the only place the float is told. The
    // chrome and the rasterised timeline are keyed on the ratio, but nothing
    // would ask for the repaint that rebuilds them after a monitor move.
    if (event->type() == QEvent::DevicePixelRatioChange ||
        event->type() == QEvent::ScreenChangeInternal) {
        timelineCacheValid_ = false;
        chromeDirtyAll();
        fitSurfaces();
    }
    return QWidget::event(event);
}

void PipFloat::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange) {
        retranslateUi();
    } else if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange ||
               event->type() == QEvent::FontChange ||
               event->type() == QEvent::ApplicationPaletteChange ||
               event->type() == QEvent::ApplicationFontChange ||
               event->type() == QEvent::LayoutDirectionChange) {
        // The float's controls take their colours, shape and metrics from the
        // application's theme; leaving their cached pixels standing would keep
        // the previous revision on screen.
        chromeDirtyAll();
    }
}

void PipFloat::retranslateUi() {
    // Every label is rebuilt from retained state: the transport and mute hints
    // carry the state they describe, so they are rebuilt with it. Nothing here
    // reaches the float's core - a wording change is not a pause, a seek or a
    // volume change - and no control is shown, hidden or moved.
    setWindowTitle(tr("Picture in picture"));
    // An IconButton copies its tooltip into an accessible name when it is
    // built, so both are restated from one string: an announced name is a label
    // too, and a stale one reads out in the previous language.
    const auto relabel = [](IconButton* button, const QString& text) {
        button->setToolTip(text);
        button->setAccessibleName(text);
    };
    relabel(playButton_, playing_ ? tr("Pause (Space)") : tr("Play (Space)"));
    updateMuteChrome();
    relabel(restoreButton_, tr("Return to the window"));
    relabel(closeButton_, tr("Close (Esc)"));
    // The clocks are digits rather than words, but they are re-rendered from
    // the retained position and length so the row cannot keep a value from
    // before the change. Left alone mid-drag: the pointer owns the timeline.
    if (!scrubbing_) {
        setTime(lastPosition_, lastDuration_);
    }
    // The chrome is drawn by the picture's painter, so the labels land through
    // the same host path every other chrome change takes.
    chromeDirtyAll();
}

void PipFloat::closeEvent(QCloseEvent* event) {
    // The float is pooled and the player owns its teardown. A compositor close
    // must take the same path as the chrome close button, not silently stop the
    // float while the player still believes PiP owns playback.
    event->ignore();
    if (closeRequestInProgress_) return;
    closeRequestInProgress_ = true;
    QPointer<PipFloat> self(this);
    emit closeRequested();
    if (self) self->closeRequestInProgress_ = false;
}

// ── the gesture on the picture ───────────────────────────────────────────

void PipFloat::pointerPress(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    pressGlobal_ = event->globalPosition();
    pressWindow_ = pos();
    pressSize_ = size();
    // The watched overlay fills the float; local coordinates remain valid even
    // when a Wayland compositor does not expose global top-level positions.
    const QPoint local = event->position().toPoint();
    resizingRight_ = local.x() >= width() - 8;
    resizingBottom_ = local.y() >= height() - 8;
    pressed_ = true;
    dragging_ = false;
    if (isWayland() && (resizingRight_ || resizingBottom_)) {
        // A resize edge is unambiguous at press time. Start the compositor
        // operation while the pointer-button serial is current.
        Qt::Edges edges;
        if (resizingRight_) edges |= Qt::RightEdge;
        if (resizingBottom_) edges |= Qt::BottomEdge;
        if (QWindow* window = windowHandle()) window->startSystemResize(edges);
        dragging_ = true;
    }
}

void PipFloat::pointerMove(QMouseEvent* event) {
    if (draggingTimeline_) {
        timelineSeekTo(event->position().x());
        return;
    }
    if (!pressed_ || !(event->buttons() & Qt::LeftButton)) {
        const QPoint local = event->position().toPoint();
        const bool right = local.x() >= width() - 8;
        const bool bottom = local.y() >= height() - 8;
        overlay_->setCursor(right && bottom ? Qt::SizeFDiagCursor
                            : right ? Qt::SizeHorCursor
                            : bottom ? Qt::SizeVerCursor : Qt::ArrowCursor);
        return;
    }
    const QPointF delta = event->globalPosition() - pressGlobal_;
    if (!dragging_) {
        if (std::abs(delta.x()) + std::abs(delta.y()) <= kPipDragThresholdPx) {
            return;
        }
        dragging_ = true;
        if (isWayland()) {
            // Preserve click-to-pause by waiting for a drag threshold. The
            // operation still belongs to this active pointer-button gesture.
            // Do not fall back to client positioning if it is refused.
            if (QWindow* window = windowHandle()) window->startSystemMove();
            return;
        }
    }
    if (isWayland()) return;
    if (resizingRight_ || resizingBottom_) {
        QScreen* target = QGuiApplication::screenAt(pressWindow_);
        if (target == nullptr) target = screen();
        const QSize bounds = target != nullptr
            ? target->availableGeometry().size() : pressSize_;
        const int width = resizingRight_
            ? std::clamp(pressSize_.width() + static_cast<int>(std::lround(delta.x())),
                         minimumWidth(), std::max(minimumWidth(), bounds.width()))
            : pressSize_.width();
        const int height = resizingBottom_
            ? std::clamp(pressSize_.height() + static_cast<int>(std::lround(delta.y())),
                         minimumHeight(), std::max(minimumHeight(), bounds.height()))
            : pressSize_.height();
        resize(width, height);
        return;
    }
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
    const bool resized = resizingRight_ || resizingBottom_;
    const bool dragged = dragging_;
    pressed_ = false;
    dragging_ = false;
    resizingRight_ = false;
    resizingBottom_ = false;
    if (draggingTimeline_) {
        draggingTimeline_ = false;
        scrubbing_ = false;
        revealControls();
        return;
    }
    if (resized) return;
    if (dragged) {
        if (!overlapsAnyWorkArea()) {
            pullIntoWorkArea();
        }
        return;
    }
    surface_->togglePause();
}

// ── placement ────────────────────────────────────────────────────────────

void PipFloat::moveToCorner(QScreen* target) {
    // Top-level placement is compositor-controlled on Wayland.
    if (target == nullptr || isWayland()) {
        return;
    }
    // Only ever the FIRST placement of a brand-new float: a sensible default
    // corner to open in before the viewer has placed it anywhere. A pooled float
    // is never sent back here on re-entry - it keeps wherever the viewer last
    // put it.
    //
    // availableGeometry, not geometry: the taskbar and the docked app bar are
    // not places a float can be dragged back from, so a window parked on the
    // work area is one the viewer cannot reach.
    const QRect area = target->availableGeometry();
    move(area.right() - width() - kPipBoundsOffset + 1,
         area.bottom() - height() - kPipBoundsOffset + 1);
}

/// The float is placed freely, so the only placement rule is reachability: some
/// part of the float must sit on a work area, or a viewer cannot click it and
/// cannot drag it back. Off-screen is the one position that has no recovery.
bool PipFloat::overlapsAnyWorkArea() const {
    const QRect frame = frameGeometry();
    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* screen : screens) {
        if (screen != nullptr && frame.intersects(screen->availableGeometry())) {
            return true;
        }
    }
    return false;
}

void PipFloat::pullIntoWorkArea() {
    if (isWayland()) return;
    // Pull the float back to the work area nearest where it already is, keeping
    // as much of its position as reachability allows: a monitor change can leave
    // the pooled position naming a screen that is gone, and a window there
    // cannot be clicked at all. availableGeometry, not geometry: the taskbar and
    // the docked app bar are not places a float can be dragged back from.
    QScreen* target = QGuiApplication::screenAt(frameGeometry().center());
    if (target == nullptr) {
        target = QGuiApplication::primaryScreen();
    }
    if (target == nullptr) {
        return;
    }
    const QRect area = target->availableGeometry();
    const QPoint origin = frameGeometry().topLeft();
    const int x = std::clamp(origin.x(), area.left(), std::max(area.left(), area.right() - width()));
    const int y =
        std::clamp(origin.y(), area.top(), std::max(area.top(), area.bottom() - height()));
    move(x, y);
}

void PipFloat::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    fitSurfaces();
    revealControls();
    // T-207: the pooled float's pump runs again from the moment it is visible;
    // see hideEvent for why it was parked.
    if (surface_ != nullptr) surface_->setPumpSuspended(false);
    // A pooled float re-enters where the viewer last left it. The one position
    // that is not honoured is one that is no longer reachable - a monitor that
    // went away since the last session - and only that is corrected, once, on
    // entry. A viewer who left it somewhere odd gets it back exactly there.
    if (!overlapsAnyWorkArea()) {
        pullIntoWorkArea();
    }
}

void PipFloat::hideEvent(QHideEvent* event) {
    pressed_ = false;
    dragging_ = false;
    resizingRight_ = false;
    resizingBottom_ = false;
    draggingTimeline_ = false;
    scrubbing_ = false;
    QWidget::hideEvent(event);
    // T-207: a hidden float is a pooled, paused second core that nothing
    // watches, yet its 50 ms pump kept draining that core's events for the rest
    // of the session - background GUI-thread work whose only effect was jitter
    // in the main player's frame schedule. The float is deliberately kept alive
    // (destroying a second QOpenGLWidget beside the player's faulted the driver),
    // but the pump is pure Qt work and stopping it is safe at any pool boundary:
    // the next drain picks up whatever the paused core queued, in the same
    // bounded batch as always. A native transition or an engine build in flight
    // still needs its observer, so those keep the pump running.
    if (surface_ != nullptr &&
        surface_->nativePlaybackStatus().pendingRequestId.isEmpty() &&
        !surface_->animeJanaiEngineBuildActive()) {
        surface_->setPumpSuspended(true);
    }
}

// ── layout ───────────────────────────────────────────────────────────────

void PipFloat::fitSurfaces() {
    surface_->setGeometry(rect());
    overlay_->setGeometry(rect());

    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0) {
        return;
    }

    // Top-left: the volume pill. Sized to its contents and placed with 12px margin.
    if (volumeChip_ != nullptr) {
        volumeChip_->adjustSize();
        volumeChip_->move(12, 12);
    }

    // Top-right: window controls pill matching volume pill geometry and height
    if (windowControls_ != nullptr) {
        windowControls_->adjustSize();
        windowControls_->move(w - 12 - windowControls_->width(), 12);
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
        remaining_->move(qMax(0, w - kSpacingMedium - remaining_->width()), labelY);
    }
}

void PipFloat::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    fitSurfaces();
    // The chrome lives in the picture now, and a resize moves every control to
    // a new place - and can land with no frame arriving. The surface draws
    // itself rather than waiting for one of those.
    surface_->update();
}

// ── keyboard ─────────────────────────────────────────────────────────────

bool PipFloat::handleKey(QKeyEvent* event) {
    switch (event->key()) {
        case Qt::Key_Left:
            // A step is a distance from where the float's own core is, not
            // from a position this side sampled up to a pump tick ago.
            surface_->seekRelative(-kPipSeekStepSeconds);
            break;
        case Qt::Key_Right:
            surface_->seekRelative(kPipSeekStepSeconds);
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


}  // namespace shell
}  // namespace mplayer
