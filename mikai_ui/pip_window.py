"""A frameless, always-on-top mini player.

The float is a picture, not a window around one: the video fills every pixel and
the controls are translucent pills floating on top of it, the way a phone player
does. Chrome that occupies space would shrink the only thing the float is for,
so there is no permanent band at the bottom and no caption anywhere.

A click on the picture is play/pause and a press-and-hold is a move. Both halves
of that gesture are tracked in screen coordinates, because the drag has to
follow the pointer exactly: converting each step back into widget space would
add a rounding error per event and the float would crawl away from the cursor.
"""

from __future__ import annotations

from PySide6.QtCore import (
    QEasingCurve,
    QEvent,
    QPoint,
    QPointF,
    QSignalBlocker,
    Qt,
    QTimer,
    QVariantAnimation,
    Signal,
)
from PySide6.QtGui import QKeyEvent, QPainterPath, QRegion
from PySide6.QtWidgets import (
    QGraphicsOpacityEffect,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSlider,
    QVBoxLayout,
    QWidget,
)

from mikai_ui.theme.colors import GHOST_HOVER, MUTED, SURFACE_ACTIVE, TEXT
from mikai_ui.theme.metrics import (
    ICON_SIZE,
    PILL_RADIUS,
    PLAYER_FIELD_HEIGHT,
    SPACING_SMALL,
    SPACING_XS,
    TIMELINE_GROOVE_HEIGHT,
    TIMELINE_HANDLE_SIZE,
    VOLUME_GROOVE_HEIGHT,
    VOLUME_HANDLE_SIZE,
)
from mikai_ui.theme.widgets import make_icon

SEEK_STEP_SECONDS = 10
CORNER_RADIUS_PX = 16
# theme.metrics.ANIMATION_DURATION_MS is gone, and no shared token would fit
# anyway: this is the only animation the float owns, and the retire delay is a
# rule about the pointer going still, not a decoration other surfaces should
# inherit
BAR_FADE_MS = 160
BAR_REVEAL_HOLD_MS = 1600
# a press that travels further than this is a move, not a click on the picture
DRAG_THRESHOLD_PX = 6
CENTER_BUTTON_SIZE = 72
# QSS paints the hairline inside the widget, so a pill has to be a border wider
# than the row it holds or the controls sit a pixel out of their own edges
PILL_HEIGHT = PLAYER_FIELD_HEIGHT + 2 * SPACING_XS + 2
VOLUME_SLIDER_WIDTH = 110
# the pills sit on live video, so the fill is translucent and the border is a
# hairline: enough to read as chrome, not enough to hide the picture under it
PILL_FILL = "rgba(24, 25, 28, 200)"
PILL_BORDER = "rgba(231, 231, 234, 0.10)"

_PILL_QSS = f"""
QWidget#pipOverlay {{ background: transparent; border: none; }}
QWidget#pipPill, QWidget#pipBar {{
    background: {PILL_FILL};
    border: 1px solid {PILL_BORDER};
    border-radius: {PILL_RADIUS}px;
}}
QLabel#pipElapsed, QLabel#pipRemaining {{
    background: transparent; color: {MUTED}; font-size: 12px; font-family: monospace;
}}
QPushButton#pipPlay {{
    background: {PILL_FILL}; border: 1px solid {PILL_BORDER};
    border-radius: {CENTER_BUTTON_SIZE // 2}px; padding: 0;
    /* the shared sheet caps #pipPlay at a 30px field for the main player's
       bar; the float's centre button is a target, not a field, so it needs
       the round size said out loud or it renders as a 72x32 lozenge */
    min-width: {CENTER_BUTTON_SIZE}px; max-width: {CENTER_BUTTON_SIZE}px;
    min-height: {CENTER_BUTTON_SIZE}px; max-height: {CENTER_BUTTON_SIZE}px;
}}
QPushButton#pipMute, QPushButton#pipRestore, QPushButton#pipClose {{
    background: transparent; border: 1px solid transparent;
    border-radius: {PILL_RADIUS // 2}px; padding: 0;
}}
QPushButton#pipPlay:hover, QPushButton#pipMute:hover, QPushButton#pipRestore:hover,
QPushButton#pipClose:hover {{ background: {GHOST_HOVER}; }}
QSlider#pipVolume, QSlider#pipScrub {{ background: transparent; }}
QSlider#pipVolume::groove:horizontal {{
    height: {VOLUME_GROOVE_HEIGHT}px; background: {SURFACE_ACTIVE}; border-radius: 2px; }}
QSlider#pipVolume::sub-page:horizontal {{ background: {MUTED}; border-radius: 2px; }}
QSlider#pipVolume::handle:horizontal {{
    background: {TEXT}; width: {VOLUME_HANDLE_SIZE}px; height: {VOLUME_HANDLE_SIZE}px;
    margin: {VOLUME_GROOVE_HEIGHT // 2 - VOLUME_HANDLE_SIZE // 2}px 0;
    border-radius: {VOLUME_HANDLE_SIZE // 2}px; }}
QSlider#pipScrub::groove:horizontal {{
    height: {TIMELINE_GROOVE_HEIGHT}px; background: {SURFACE_ACTIVE}; border-radius: 2px; }}
QSlider#pipScrub::sub-page:horizontal {{ background: {MUTED}; border-radius: 2px; }}
QSlider#pipScrub::handle:horizontal {{
    background: {TEXT}; width: {TIMELINE_HANDLE_SIZE}px; height: {TIMELINE_HANDLE_SIZE}px;
    margin: {TIMELINE_GROOVE_HEIGHT // 2 - TIMELINE_HANDLE_SIZE // 2}px 0;
    border-radius: {TIMELINE_HANDLE_SIZE // 2}px; }}
"""


def _format_seconds(value: float) -> str:
    """Seconds as m:ss, gaining an hours field only when it is needed."""
    total = max(0, int(value))
    hours, rest = divmod(total, 3600)
    minutes, seconds = divmod(rest, 60)
    if hours:
        return f"{hours}:{minutes:02d}:{seconds:02d}"
    return f"{minutes}:{seconds:02d}"


class _Overlay(QWidget):
    """The full-rect surface the controls float on and the pointer talks to.

    It is a NATIVE child so it is stacked above the video's own native surface:
    a plain widget would be painted underneath whatever the decoder owns. The
    pointer never reaches the picture itself, so the click and the drag both
    arrive here in one place.
    """

    def __init__(self, pip: PipWindow) -> None:
        super().__init__(pip)
        self._pip = pip
        # the ancestor has to stop creating native windows first, otherwise the
        # video surface is promoted to native and ends up above the overlay
        self.setAttribute(Qt.WidgetAttribute.WA_DontCreateNativeAncestors)
        self.setAttribute(Qt.WidgetAttribute.WA_NativeWindow)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMouseTracking(True)

    def mousePressEvent(self, event) -> None:  # noqa: N802 - Qt naming
        self._pip._pointer_press(event)

    def mouseMoveEvent(self, event) -> None:  # noqa: N802 - Qt naming
        self._pip._pointer_move(event)

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802 - Qt naming
        self._pip._pointer_release(event)

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802 - Qt naming
        if not self._pip._handle_key(event):
            super().keyPressEvent(event)


class PipWindow(QWidget):
    """Frameless float: the video full bleed, the controls floating on it."""

    togglePlayRequested = Signal()
    seekRequested = Signal(int)
    seekAbsoluteRequested = Signal(float)
    volumeChanged = Signal(float)
    muteChanged = Signal(bool)
    restoreRequested = Signal()
    closeRequested = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(
            None,
            Qt.WindowType.Tool
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint,
        )
        self.setObjectName("playerPip")
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMouseTracking(True)

        self._video: QWidget | None = None
        self._muted = False
        self._scrubbing = False
        # the press that started the possible drag, and where the window was when
        # it did: the whole move is measured between these two screen positions
        self._press_global: QPointF | None = None
        self._press_window = QPoint()
        self._dragging = False

        self._video_holder = QWidget(self)
        self._video_holder.setObjectName("pipVideo")
        self._video_holder.setMouseTracking(True)

        self._overlay = _Overlay(self)
        self._overlay.setObjectName("pipOverlay")
        self._overlay.setStyleSheet(_PILL_QSS)
        overlay_layout = QVBoxLayout(self._overlay)
        overlay_layout.setContentsMargins(
            SPACING_SMALL, SPACING_SMALL, SPACING_SMALL, SPACING_SMALL
        )
        overlay_layout.setSpacing(0)

        self._top_pill = self._build_pill("pipPill")
        self._top_row = QHBoxLayout(self._top_pill)
        self._set_row_margins(self._top_row)

        self._mute_button = self._build_button(
            "pipMute", "volume", "Вимкнути звук", PLAYER_FIELD_HEIGHT
        )
        self._top_row.addWidget(self._mute_button)
        self._mute_button.clicked.connect(self._on_mute_clicked)

        self._volume_slider = QSlider(Qt.Orientation.Horizontal, self._top_pill)
        self._volume_slider.setObjectName("pipVolume")
        # without NoFocus the slider eats Space, so the shortcut that works in
        # the main player would do nothing here
        self._volume_slider.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._volume_slider.setRange(0, 100)
        self._volume_slider.setFixedWidth(VOLUME_SLIDER_WIDTH)
        self._volume_slider.valueChanged.connect(self._on_volume_changed)
        self._top_row.addWidget(self._volume_slider)
        self._top_row.addStretch(1)

        self._restore_button = self._build_button(
            "pipRestore", "windowed", "Повернути у вікно", PLAYER_FIELD_HEIGHT
        )
        self._restore_button.clicked.connect(self.restoreRequested)
        self._top_row.addWidget(self._restore_button)

        self._close_button = self._build_button(
            "pipClose", "close", "Закрити (Esc)", PLAYER_FIELD_HEIGHT
        )
        self._close_button.clicked.connect(self.closeRequested)
        self._top_row.addWidget(self._close_button)

        self._play_button = QPushButton(self._overlay)
        self._play_button.setObjectName("pipPlay")
        self._play_button.setFixedSize(CENTER_BUTTON_SIZE, CENTER_BUTTON_SIZE)
        self._play_button.setIcon(make_icon("pause", size=ICON_SIZE))
        self._play_button.setToolTip("Пауза (Пробіл)")
        self._play_button.setAccessibleName("Пауза")
        self._play_button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._play_button.clicked.connect(self.togglePlayRequested)

        self._bar = self._build_pill("pipBar")
        self._bottom_row = QHBoxLayout(self._bar)
        self._set_row_margins(self._bottom_row)

        self._elapsed = QLabel("0:00", self._bar)
        self._elapsed.setObjectName("pipElapsed")
        self._bottom_row.addWidget(self._elapsed)

        self._scrub = QSlider(Qt.Orientation.Horizontal, self._bar)
        self._scrub.setObjectName("pipScrub")
        self._scrub.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._scrub.setRange(0, 1)
        self._scrub.sliderPressed.connect(self._on_scrub_pressed)
        self._scrub.sliderReleased.connect(self._on_scrub_released)
        self._bottom_row.addWidget(self._scrub, 1)

        self._remaining = QLabel("--:--", self._bar)
        self._remaining.setObjectName("pipRemaining")
        self._bottom_row.addWidget(self._remaining)

        # the overlay is full rect, so the two pills and the centre button stack
        # over the picture instead of taking a row of it
        overlay_layout.addWidget(self._top_pill, 0, Qt.AlignmentFlag.AlignTop)
        overlay_layout.addStretch(1)
        overlay_layout.addWidget(self._play_button, 0, Qt.AlignmentFlag.AlignCenter)
        overlay_layout.addStretch(1)
        overlay_layout.addWidget(self._bar, 0, Qt.AlignmentFlag.AlignBottom)

        # the controls are chrome drawn over a picture, so they belong to the
        # pointer and to the focus rather than sitting on top of another app
        # forever
        self._overlay_opacity = 1.0
        self._overlay_effect = QGraphicsOpacityEffect(self._overlay)
        self._overlay_effect.setOpacity(1.0)
        self._overlay.setGraphicsEffect(self._overlay_effect)
        self._overlay_fade = QVariantAnimation(self)
        self._overlay_fade.setDuration(BAR_FADE_MS)
        self._overlay_fade.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._overlay_fade.valueChanged.connect(self._set_overlay_opacity)
        self._hide_timer = QTimer(self)
        self._hide_timer.setSingleShot(True)
        self._hide_timer.timeout.connect(self._hide_controls)

        for watched in (
            self,
            self._video_holder,
            self._overlay,
            self._top_pill,
            self._bar,
            self._elapsed,
            self._remaining,
            self._volume_slider,
            self._scrub,
            self._play_button,
            self._mute_button,
            self._restore_button,
            self._close_button,
        ):
            watched.installEventFilter(self)

    # -- construction helpers -------------------------------------------
    def _build_pill(self, name: str) -> QWidget:
        pill = QWidget(self._overlay)
        pill.setObjectName(name)
        pill.setFixedHeight(PILL_HEIGHT)
        pill.setMouseTracking(True)
        return pill

    def _build_button(self, name: str, icon: str, hint: str, size: int) -> QPushButton:
        button = QPushButton(self)
        button.setObjectName(name)
        button.setFixedSize(size, size)
        button.setIcon(make_icon(icon, size=ICON_SIZE))
        button.setToolTip(hint)
        button.setAccessibleName(hint)
        button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        button.setMouseTracking(True)
        return button

    @staticmethod
    def _set_row_margins(layout: QHBoxLayout) -> None:
        layout.setContentsMargins(SPACING_SMALL, SPACING_XS, SPACING_SMALL, SPACING_XS)
        layout.setSpacing(SPACING_SMALL)

    # -- content --------------------------------------------------------
    def set_video(self, widget: QWidget) -> None:
        """Adopt the player's video surface and remember it for the way back."""
        self._video = widget
        self._video_holder.installEventFilter(self)
        widget.installEventFilter(self)
        widget.setParent(self._video_holder)
        widget.setMouseTracking(True)
        self._fit_surfaces()
        widget.show()
        # the surface was just promoted and lands above its new siblings
        self._overlay.raise_()

    def release_video(self) -> QWidget | None:
        video, self._video = self._video, None
        return video

    def set_playing(self, playing: bool) -> None:
        icon = "pause" if playing else "play"
        hint = "Пауза (Пробіл)" if playing else "Продовжити (Пробіл)"
        self._play_button.setIcon(make_icon(icon, size=ICON_SIZE))
        self._play_button.setToolTip(hint)
        self._play_button.setAccessibleName(hint)

    def set_time(self, position: float, duration: float) -> None:
        """Show the position the player reports and the room left before the end.

        Nothing is emitted: this is the player telling the float where it is,
        and answering with a seek request would loop straight back.
        """
        if duration > 0:
            self._elapsed.setText(_format_seconds(position))
            self._remaining.setText("-" + _format_seconds(max(0.0, duration - position)))
        else:
            # a live stream has no end to count down to, and a slider over an
            # empty range would report every set as zero
            self._elapsed.setText("--:--")
            self._remaining.setText("--:--")
        self._scrub.setRange(0, max(1, int(round(duration * 1000))))
        if not self._scrubbing:
            clamped = position if duration > 0 else 0.0
            self._scrub.setValue(int(round(max(0.0, min(clamped, duration)) * 1000)))

    def set_volume(self, value: float) -> None:
        with QSignalBlocker(self._volume_slider):
            self._volume_slider.setValue(int(round(max(0.0, min(1.0, float(value))) * 100)))

    def set_muted(self, muted: bool) -> None:
        self._muted = bool(muted)
        hint = "Увімкнути звук" if muted else "Вимкнути звук"
        self._mute_button.setIcon(make_icon("volume_muted" if muted else "volume", size=ICON_SIZE))
        self._mute_button.setToolTip(hint)
        self._mute_button.setAccessibleName(hint)

    # -- what the controls say back -------------------------------------
    def _on_mute_clicked(self) -> None:
        self.set_muted(not self._muted)
        self._reveal_controls()
        self.muteChanged.emit(self._muted)

    def _on_volume_changed(self, value: int) -> None:
        self._reveal_controls()
        # the slider's own 0…100 range is the clamp: re-clamping here would only
        # be a second copy of a rule the widget already enforces
        self.volumeChanged.emit(value / 100.0)

    def _on_scrub_pressed(self) -> None:
        self._scrubbing = True
        self._reveal_controls()

    def _on_scrub_released(self) -> None:
        self._scrubbing = False
        self._reveal_controls()
        self.seekAbsoluteRequested.emit(self._scrub.value() / 1000.0)

    # -- the controls come and go with the pointer and the focus ---------
    @property
    def controls_shown(self) -> bool:
        return self._overlay_opacity > 0.01

    def _set_overlay_opacity(self, value: float) -> None:
        self._overlay_opacity = float(value)
        self._overlay_effect.setOpacity(self._overlay_opacity)

    def _reveal_controls(self) -> None:
        # the reveal is instant on purpose: a fade in makes the controls feel
        # late to a pointer that is already on the picture
        self._hide_timer.stop()
        self._overlay_fade.stop()
        self._set_overlay_opacity(1.0)
        # a hover can reach a float the user never activated, so the controls
        # have to retire again on their own rather than wait for a focus event
        # that may never arrive
        self._hide_timer.start(BAR_REVEAL_HOLD_MS)

    def _hide_controls(self) -> None:
        if self._overlay_opacity > 0.01:
            self._overlay_fade.stop()
            self._overlay_fade.setStartValue(self._overlay_opacity)
            self._overlay_fade.setEndValue(0.0)
            self._overlay_fade.start()

    def eventFilter(self, watched, event) -> bool:  # noqa: N802 - Qt naming
        """Keep the controls in step with the pointer and with the activation.

        A click on the picture is a command, so the overlay has to see it; the
        gesture itself lives in the overlay's own mouse handlers, which the
        children never let past.
        """
        kind = event.type()
        if kind in (
            QEvent.Type.MouseMove,
            QEvent.Type.MouseButtonPress,
            QEvent.Type.MouseButtonRelease,
            QEvent.Type.Wheel,
        ):
            self._reveal_controls()
        if watched is self:
            # Win32 delivers activation to the top level, and a changeEvent
            # override never sees it, so the float watches its own events
            if kind == QEvent.Type.WindowActivate:
                self._reveal_controls()
            elif kind == QEvent.Type.WindowDeactivate:
                self._hide_timer.stop()
                self._hide_controls()
        return super().eventFilter(watched, event)

    def showEvent(self, event) -> None:  # noqa: N802 - Qt naming
        super().showEvent(event)
        self._fit_surfaces()
        self._reveal_controls()

    # -- the gesture on the picture --------------------------------------
    def _pointer_press(self, event) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        # both anchors are taken in screen space: the pointer position it was
        # grabbed at, and where the window itself was sitting when it was
        self._press_global = QPointF(event.globalPosition())
        self._press_window = self.pos()
        self._dragging = False

    def _pointer_move(self, event) -> None:
        if self._press_global is None or not event.buttons() & Qt.MouseButton.LeftButton:
            return
        delta = event.globalPosition() - self._press_global
        if not self._dragging:
            travel = abs(delta.x()) + abs(delta.y())
            if travel <= DRAG_THRESHOLD_PX:
                return
            self._dragging = True
        # the window is placed from where it started plus where the pointer has
        # come to, not nudged by the last event: the two operands are both
        # screen positions, so nothing rounds twice and the float stays exactly
        # under the cursor
        self.move(self._press_window + QPoint(int(round(delta.x())), int(round(delta.y()))))

    def _pointer_release(self, event) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        dragged, self._dragging = self._dragging, False
        self._press_global = None
        if not dragged:
            self.togglePlayRequested.emit()

    # -- layout ----------------------------------------------------------
    def _fit_surfaces(self) -> None:
        """The video and the overlay both claim the whole window.

        Nothing is reserved for chrome: a row left empty at the bottom would be
        a row of black the user watches instead of the picture.
        """
        rect = self.rect()
        self._video_holder.setGeometry(rect)
        if self._video is not None:
            self._video.setGeometry(self._video_holder.rect())
        self._overlay.setGeometry(rect)
        self._overlay.raise_()

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt naming
        super().resizeEvent(event)
        self._apply_corner_mask()
        self._fit_surfaces()

    def _apply_corner_mask(self) -> None:
        """Round the window itself, not just its background.

        A styled border-radius only clips the widget's own painting, and the
        video is a NATIVE child window: without a mask on the top-level it
        paints square corners straight over the rounded background.
        """
        if self.width() <= 0 or self.height() <= 0:
            return
        path = QPainterPath()
        path.addRoundedRect(self.rect(), CORNER_RADIUS_PX, CORNER_RADIUS_PX)
        # QRegion takes QPoints here, not the QPolygonF path hands back
        points = [point.toPoint() for polygon in path.toSubpathPolygons() for point in polygon]
        self.setMask(QRegion(points))

    # -- keyboard --------------------------------------------------------
    def _handle_key(self, event: QKeyEvent) -> bool:
        """The keys the float answers, wherever the focus happens to sit."""
        key = event.key()
        if key == Qt.Key.Key_Left:
            self.seekRequested.emit(-SEEK_STEP_SECONDS)
        elif key == Qt.Key.Key_Right:
            self.seekRequested.emit(SEEK_STEP_SECONDS)
        elif key == Qt.Key.Key_Space:
            self.togglePlayRequested.emit()
        elif key == Qt.Key.Key_Escape:
            self.closeRequested.emit()
        else:
            return False
        self._reveal_controls()
        return True

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802 - Qt naming
        if not self._handle_key(event):
            super().keyPressEvent(event)
