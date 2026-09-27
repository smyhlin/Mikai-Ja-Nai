"""Reusable transport and playback controls with command-only signals."""

from __future__ import annotations

from PySide6.QtCore import QSize, Qt, Signal
from PySide6.QtWidgets import (
    QComboBox,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSlider,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from mikai_ui.player_controller import PlaybackState, PlaybackStatus
from mikai_ui.theme.colors import ICON_DISABLED
from mikai_ui.theme.metrics import (
    ICON_SIZE,
    PLAYER_FIELD_MIN_WIDTH,
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
    """A normalized timeline that previews during drag and commits on release."""

    seekStarted = Signal()
    seekPreviewChanged = Signal(float)
    seekFinished = Signal(float)

    SCALE = TIMELINE_SCALE

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(Qt.Orientation.Horizontal, parent)
        self._duration = 0.0
        self.setObjectName("timeline")
        self.setRange(0, self.SCALE)
        self.setTracking(False)
        self.setFixedHeight(TIMELINE_ROW_HEIGHT)
        self.setAccessibleName("Позиція відтворення")
        self.setToolTip("Перемотування відео")
        self.sliderPressed.connect(self._begin_seek)
        self.sliderMoved.connect(self._preview_value)
        self.sliderReleased.connect(self._finish_seek)

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

    def _begin_seek(self) -> None:
        if self._duration > 0:
            self.seekStarted.emit()
            self._preview_value(self.value())

    def _preview_value(self, value: int) -> None:
        if self._duration > 0:
            self.seekPreviewChanged.emit(self.position_for_value(value))

    def _finish_seek(self) -> None:
        if self._duration > 0:
            self.seekFinished.emit(self.position_for_value(self.value()))


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

        timeline_row = QHBoxLayout()
        timeline_row.setSpacing(SPACING_MEDIUM)
        self.timeline = TimelineSlider(self)
        self.time_label = QLabel("0:00 / --:--")
        self.time_label.setObjectName("playerTime")
        self.time_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        self.time_label.setMinimumWidth(TIME_LABEL_MIN_WIDTH)
        self.timeline.seekStarted.connect(self.seekStarted)
        self.timeline.seekPreviewChanged.connect(self._show_preview)
        self.timeline.seekPreviewChanged.connect(self.seekPreviewChanged)
        self.timeline.seekFinished.connect(self._finish_preview)
        self.timeline.seekFinished.connect(self.seekFinished)
        timeline_row.addWidget(self.timeline, 1)
        timeline_row.addWidget(self.time_label)
        root.addLayout(timeline_row)

        controls = QHBoxLayout()
        controls.setSpacing(SPACING_SMALL)
        self.rewind_button = self._button("Назад на 10 секунд (←)", secondary=True)
        self.rewind_button.setText("−10")
        self.rewind_button.clicked.connect(self.rewindRequested)
        self.play_button = self._button("Пауза (Пробіл)", secondary=False)
        self.play_button.setIcon(make_icon("pause", size=ICON_SIZE, disabled_color=ICON_DISABLED))
        self.play_button.clicked.connect(self.playPauseRequested)
        self.forward_button = self._button("Вперед на 10 секунд (→)", secondary=True)
        self.forward_button.setText("+10")
        self.forward_button.clicked.connect(self.forwardRequested)
        controls.addWidget(self.rewind_button)
        controls.addWidget(self.play_button)
        controls.addWidget(self.forward_button)
        controls.addStretch(1)

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
    def _button(tooltip: str, *, secondary: bool) -> QPushButton:
        button = QPushButton()
        button.setObjectName("playerSecondary" if secondary else "playerPrimary")
        button.setIconSize(QSize(ICON_SIZE, ICON_SIZE))
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
                size=ICON_SIZE,
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
