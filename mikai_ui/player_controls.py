"""Reusable transport and playback controls with command-only signals."""

from __future__ import annotations

from PySide6.QtCore import QSize, Qt, Signal
from PySide6.QtGui import QFont
from PySide6.QtWidgets import (
    QComboBox,
    QToolTip,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSlider,
    QSizePolicy,
    QStyledItemDelegate,
    QVBoxLayout,
    QWidget,
)

from mikai_ui.player_controller import PlaybackState, PlaybackStatus
from mikai_ui.theme.colors import ICON_DISABLED
from mikai_ui.theme.metrics import (
    ICON_SIZE,
    PLAYER_DIVIDER_HEIGHT,
    PLAYER_PRIMARY_ICON_SIZE,
    PLAYER_SKIP_WIDTH,
    PLAYER_FIELD_MIN_WIDTH,
    PLAYER_FIELD_ROW_HEIGHT,
    PRIMARY_CONTROL_SIZE,
    SECONDARY_CONTROL_SIZE,
    SPACING_MEDIUM,
    SPACING_SMALL,
    TIME_LABEL_MIN_WIDTH,
    TIMELINE_ROW_HEIGHT,
    TIMELINE_SCALE,
    VOLUME_SLIDER_WIDTH,
)
from mikai_ui.theme.widgets import make_icon


def format_time(seconds: float) -> str:
    value = max(0, int(seconds or 0))
    hours, remainder = divmod(value, 3600)
    minutes, secs = divmod(remainder, 60)
    return f"{hours}:{minutes:02}:{secs:02}" if hours else f"{minutes}:{secs:02}"


class TimelineSlider(QSlider):
    """A normalized timeline that previews during drag and commits on release.

    Clicking anywhere on the track jumps there and commits on release, not just
    dragging the handle. Qt's QSlider only emits sliderPressed/sliderMoved/
    sliderReleased when its own handle is grabbed, so a click on the track body
    was silently ignored and the video never moved.
    """

    seekStarted = Signal()
    seekPreviewChanged = Signal(float)
    seekFinished = Signal(float)

    SCALE = TIMELINE_SCALE

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(Qt.Orientation.Horizontal, parent)
        self._duration = 0.0
        self._scrubbing = False
        self.setMouseTracking(True)
        self.setObjectName("timeline")
        self.setRange(0, self.SCALE)
        self.setTracking(False)
        self.setFixedHeight(TIMELINE_ROW_HEIGHT)
        self.setAccessibleName("Позиція відтворення")
        self.setToolTip("Перемотування відео")

    def _value_at(self, x: float) -> int:
        """Map a click x-coordinate straight onto the timeline, so a click at 25%
        of the track lands at 25% of the video rather than half a handle off."""
        ratio = max(0.0, min(1.0, x / max(1, self.width())))
        return round(ratio * self.SCALE)

    def mousePressEvent(self, event) -> None:
        if self._duration <= 0 or event.button() != Qt.MouseButton.LeftButton:
            super().mousePressEvent(event)
            return
        self._scrubbing = True
        self.setValue(self._value_at(event.position().x()))
        self.seekStarted.emit()
        self.seekPreviewChanged.emit(self.position_for_value(self.value()))
        event.accept()

    def _show_hover(self, event) -> None:
        """Tell the user where a click will land before they commit to it."""
        if self._duration <= 0:
            return
        seconds = self.position_for_value(self._value_at(event.position().x()))
        QToolTip.showText(event.globalPosition().toPoint(), format_time(seconds), self)

    def leaveEvent(self, event) -> None:
        QToolTip.hideText()
        super().leaveEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if not self._scrubbing:
            self._show_hover(event)
            super().mouseMoveEvent(event)
            return
        self.setValue(self._value_at(event.position().x()))
        self.seekPreviewChanged.emit(self.position_for_value(self.value()))
        event.accept()

    def mouseReleaseEvent(self, event) -> None:
        if not self._scrubbing or event.button() != Qt.MouseButton.LeftButton:
            super().mouseReleaseEvent(event)
            return
        self._scrubbing = False
        self.setValue(self._value_at(event.position().x()))
        self.seekFinished.emit(self.position_for_value(self.value()))
        event.accept()

    def set_playback_position(self, position: float, duration: float) -> None:
        self._duration = max(0.0, float(duration))
        self.setEnabled(self._duration > 0)
        if self.isSliderDown() or self._duration <= 0:
            return
        ratio = max(0.0, min(1.0, float(position) / self._duration))
        self.setValue(round(ratio * self.SCALE))

    def position_for_value(self, value: int) -> float:
        if self._duration <= 0:
            return 0.0
        ratio = max(0.0, min(1.0, value / self.SCALE))
        return ratio * self._duration


class FieldRowDelegate(QStyledItemDelegate):
    """Give combo popup rows a readable height.

    Qt applies stylesheet `::item` sizing to list and table views but not to a
    QComboBox popup, so the row height has to come from the delegate.
    """

    def sizeHint(self, option, index):  # noqa: N802 - Qt naming
        hint = super().sizeHint(option, index)
        return QSize(hint.width(), max(hint.height(), PLAYER_FIELD_ROW_HEIGHT))


class PlayerControlsWidget(QWidget):
    """The player toolbar. It knows playback intent, never the mpv engine."""

    playPauseRequested = Signal()
    rewindRequested = Signal()
    forwardRequested = Signal()
    seekStarted = Signal()
    seekPreviewChanged = Signal(float)
    seekFinished = Signal(float)
    volumeChanged = Signal(int)
    muteRequested = Signal()
    speedChanged = Signal(float)
    qualityChanged = Signal(int)
    fullscreenRequested = Signal()

    SPEEDS = (0.5, 0.75, 1.0, 1.25, 1.5, 2.0)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("playerControls")
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self._duration = 0.0
        self._preview_position: float | None = None

        root = QVBoxLayout(self)
        root.setContentsMargins(SPACING_MEDIUM, SPACING_SMALL, SPACING_MEDIUM, SPACING_SMALL)
        root.setSpacing(SPACING_SMALL)

        self.timeline = TimelineSlider(self)
        self.time_label = QLabel("0:00 / --:--")
        self.time_label.setObjectName("playerTime")
        time_font = self.time_label.font()
        time_font.setFeature(QFont.Tag("tnum"), 1)
        self.time_label.setFont(time_font)
        self.time_label.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        self.time_label.setMinimumWidth(TIME_LABEL_MIN_WIDTH)
        self.timeline.seekStarted.connect(self.seekStarted)
        self.timeline.seekPreviewChanged.connect(self._show_preview)
        self.timeline.seekPreviewChanged.connect(self.seekPreviewChanged)
        self.timeline.seekFinished.connect(self._finish_preview)
        self.timeline.seekFinished.connect(self.seekFinished)
        root.addWidget(self.timeline)

        controls = QHBoxLayout()
        controls.setSpacing(SPACING_SMALL)
        self.rewind_button = self._skip_button("Назад на 10 секунд (←)", "rewind")
        self.rewind_button.clicked.connect(self.rewindRequested)
        self.play_button = self._button("Пауза (Пробіл)", secondary=False)
        self.play_button.setIcon(
            make_icon("pause", size=PLAYER_PRIMARY_ICON_SIZE, disabled_color=ICON_DISABLED)
        )
        self.play_button.clicked.connect(self.playPauseRequested)
        self.forward_button = self._skip_button("Вперед на 10 секунд (→)", "forward")
        self.forward_button.clicked.connect(self.forwardRequested)
        controls.addWidget(self.rewind_button)
        controls.addWidget(self.play_button)
        controls.addWidget(self.forward_button)
        controls.addSpacing(SPACING_SMALL)
        controls.addWidget(self.time_label)
        controls.addStretch(1)
        controls.addWidget(self._divider())

        self.mute_button = self._button("Вимкнути звук (M)", secondary=True)
        self.mute_button.setIcon(make_icon("volume", size=ICON_SIZE, disabled_color=ICON_DISABLED))
        self.mute_button.clicked.connect(self.muteRequested)
        controls.addWidget(self.mute_button)
        self.volume_slider = QSlider(Qt.Orientation.Horizontal)
        self.volume_slider.setObjectName("volume")
        self.volume_slider.setRange(0, 100)
        self.volume_slider.setValue(100)
        self.volume_slider.setFixedWidth(VOLUME_SLIDER_WIDTH)
        self.volume_slider.setAccessibleName("Гучність")
        self.volume_slider.setToolTip("Гучність (↑ / ↓)")
        self.volume_slider.valueChanged.connect(self.volumeChanged)
        controls.addWidget(self.volume_slider)

        self.speed_combo = QComboBox()
        self.speed_combo.setObjectName("playerField")
        self.speed_combo.setItemDelegate(FieldRowDelegate(self.speed_combo))
        self.speed_combo.setMinimumWidth(PLAYER_FIELD_MIN_WIDTH)
        self.speed_combo.setAccessibleName("Швидкість відтворення")
        self.speed_combo.setToolTip("Швидкість відтворення")
        for speed in self.SPEEDS:
            label = f"{speed:g}×"
            self.speed_combo.addItem(label, speed)
        self.speed_combo.setCurrentIndex(self.SPEEDS.index(1.0))
        self.speed_combo.currentIndexChanged.connect(self._speed_selected)
        controls.addWidget(self.speed_combo)

        self.quality_combo = QComboBox()
        self.quality_combo.setObjectName("playerField")
        self.quality_combo.setItemDelegate(FieldRowDelegate(self.quality_combo))
        self.quality_combo.setMinimumWidth(PLAYER_FIELD_MIN_WIDTH)
        self.quality_combo.setAccessibleName("Якість відео")
        self.quality_combo.setToolTip("Якість відео")
        self.quality_combo.currentIndexChanged.connect(self._quality_selected)
        self.quality_combo.hide()
        controls.addWidget(self.quality_combo)

        self.fullscreen_button = self._button("На весь екран (F)", secondary=True)
        self.fullscreen_button.clicked.connect(self.fullscreenRequested)
        controls.addWidget(self.fullscreen_button)
        root.addLayout(controls)

    @staticmethod
    def _skip_button(tooltip: str, icon_name: str) -> QPushButton:
        button = QPushButton()
        button.setObjectName("playerSkip")
        button.setIcon(make_icon(icon_name, size=ICON_SIZE, disabled_color=ICON_DISABLED))
        button.setIconSize(QSize(ICON_SIZE, ICON_SIZE))
        button.setText("10")
        button.setFixedSize(PLAYER_SKIP_WIDTH, SECONDARY_CONTROL_SIZE)
        button.setToolTip(tooltip)
        button.setAccessibleName(tooltip)
        button.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        return button

    @staticmethod
    def _divider() -> QWidget:
        line = QWidget()
        line.setObjectName("playerDivider")
        line.setFixedSize(1, PLAYER_DIVIDER_HEIGHT)
        line.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        return line

    @staticmethod
    def _button(tooltip: str, *, secondary: bool) -> QPushButton:
        button = QPushButton()
        button.setObjectName("playerSecondary" if secondary else "playerPrimary")
        icon_size = ICON_SIZE if secondary else PLAYER_PRIMARY_ICON_SIZE
        button.setIconSize(QSize(icon_size, icon_size))
        size = SECONDARY_CONTROL_SIZE if secondary else PRIMARY_CONTROL_SIZE
        button.setFixedSize(size, size)
        button.setToolTip(tooltip)
        button.setAccessibleName(tooltip)
        button.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        return button

    def set_qualities(self, qualities: dict[int, str], selected: int | None) -> None:
        self.quality_combo.blockSignals(True)
        self.quality_combo.clear()
        for height in sorted(qualities, reverse=True):
            self.quality_combo.addItem(f"{height}p", height)
        selected_index = self.quality_combo.findData(selected)
        if selected_index >= 0:
            self.quality_combo.setCurrentIndex(selected_index)
        self.quality_combo.blockSignals(False)
        self.quality_combo.setVisible(self.quality_combo.count() > 1)

    def set_fullscreen(self, fullscreen: bool) -> None:
        name = "windowed" if fullscreen else "fullscreen"
        hint = "Зігнути вікно (Esc)" if fullscreen else "На весь екран (F)"
        self.fullscreen_button.setIcon(
            make_icon(name, size=ICON_SIZE, disabled_color=ICON_DISABLED)
        )
        self.fullscreen_button.setToolTip(hint)
        self.fullscreen_button.setAccessibleName(hint)

    def set_playback_state(self, state: PlaybackState) -> None:
        self._duration = state.duration
        dragging = self.timeline.isSliderDown()
        self.timeline.set_playback_position(state.position, state.duration)
        if not dragging:
            self._preview_position = None
            self._update_time(state.position)

        unavailable = not state.available
        blocked = state.status in (
            PlaybackStatus.LOADING,
            PlaybackStatus.ERROR,
            PlaybackStatus.STOPPED,
        )
        transport_enabled = not unavailable and not blocked
        for button in (self.rewind_button, self.forward_button):
            button.setEnabled(transport_enabled)
        self.play_button.setEnabled(
            not unavailable and state.status not in (PlaybackStatus.LOADING, PlaybackStatus.ERROR)
        )
        self.timeline.setEnabled(
            not unavailable
            and state.duration > 0
            and state.status
            not in (
                PlaybackStatus.ERROR,
                PlaybackStatus.LOADING,
                PlaybackStatus.STOPPED,
            )
        )
        self.volume_slider.setEnabled(not unavailable and state.status is not PlaybackStatus.ERROR)
        self.mute_button.setEnabled(not unavailable and state.status is not PlaybackStatus.ERROR)
        self.speed_combo.setEnabled(not unavailable and state.status is not PlaybackStatus.ERROR)
        self.quality_combo.setEnabled(
            not unavailable
            and self.quality_combo.count() > 1
            and state.status is not PlaybackStatus.ERROR
        )
        paused = state.paused or state.finished
        self.play_button.setIcon(
            make_icon(
                "play" if paused else "pause",
                size=PLAYER_PRIMARY_ICON_SIZE,
                disabled_color=ICON_DISABLED,
            )
        )
        play_hint = "Пуск (Пробіл)" if paused else "Пауза (Пробіл)"
        self.play_button.setToolTip(play_hint)
        self.play_button.setAccessibleName(play_hint)
        self.mute_button.setIcon(
            make_icon(
                "volume_muted" if state.muted else "volume",
                size=ICON_SIZE,
                disabled_color=ICON_DISABLED,
            )
        )
        self.volume_slider.blockSignals(True)
        self.volume_slider.setValue(state.volume)
        self.volume_slider.blockSignals(False)
        self.speed_combo.blockSignals(True)
        speed_index = self.speed_combo.findData(state.speed)
        if speed_index >= 0:
            self.speed_combo.setCurrentIndex(speed_index)
        self.speed_combo.blockSignals(False)
        if state.quality is not None:
            quality_index = self.quality_combo.findData(state.quality)
            if quality_index >= 0:
                self.quality_combo.blockSignals(True)
                self.quality_combo.setCurrentIndex(quality_index)
                self.quality_combo.blockSignals(False)

    def _show_preview(self, position: float) -> None:
        self._preview_position = position
        self._update_time(position)

    def _finish_preview(self, position: float) -> None:
        self._preview_position = None
        self._update_time(position)

    def _update_time(self, position: float) -> None:
        total = format_time(self._duration) if self._duration > 0 else "--:--"
        self.time_label.setText(f"{format_time(position)} / {total}")

    def _speed_selected(self, index: int) -> None:
        speed = self.speed_combo.itemData(index)
        if speed is not None:
            self.speedChanged.emit(float(speed))

    def _quality_selected(self, index: int) -> None:
        height = self.quality_combo.itemData(index)
        if isinstance(height, int):
            self.qualityChanged.emit(height)
