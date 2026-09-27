"""Composition root for the embedded Mikai player."""

from __future__ import annotations

import logging
from collections.abc import Callable
from pathlib import Path

from PySide6.QtCore import QEvent, QObject, Qt, QTimer, Signal
from PySide6.QtGui import QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QAbstractButton,
    QDialog,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QVBoxLayout,
    QWidget,
)

from mikai_ui.fullscreen_manager import FullscreenManager
from mikai_ui.mpv_backend import MPVAdapter
from mikai_ui.player_controller import PlaybackState, PlaybackStatus, PlayerController
from mikai_ui.player_controls import PlayerControlsWidget, format_time
from mikai_ui.player_overlay import PlayerOverlayWidget
from mikai_ui.theme.metrics import (
    PLAYER_DEFAULT_HEIGHT,
    PLAYER_DEFAULT_WIDTH,
    PLAYER_MIN_HEIGHT,
    PLAYER_MIN_WIDTH,
    SPACING_LARGE,
    SPACING_MEDIUM,
)

log = logging.getLogger(__name__)


class PlayerDialog(QDialog):
    """Own widget composition and connect user intent to PlayerController."""

    runtimeRequested = Signal()
    playbackFailed = Signal()

    def __init__(
        self,
        caption: str,
        url: str,
        qualities: dict[int, str] | None = None,
        initial_height: int | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._caption = caption
        self._url = url
        self._qualities = dict(qualities or {})
        self._runtime_dir: Path | None = None
        self._closed = False
        self._showing = False
        self._refresh_attempted = False

        self.setObjectName("playerWindow")
        self.setWindowTitle(f"Плеєр — {caption}")
        self.setMinimumSize(PLAYER_MIN_WIDTH, PLAYER_MIN_HEIGHT)
        self.resize(PLAYER_DEFAULT_WIDTH, PLAYER_DEFAULT_HEIGHT)
        self.setContentsMargins(0, 0, 0, 0)
        self.setMouseTracking(True)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        self._header = QWidget(self)
        self._header.setObjectName("playerHeader")
        header_layout = QHBoxLayout(self._header)
        header_layout.setContentsMargins(
            SPACING_LARGE, SPACING_MEDIUM, SPACING_LARGE, SPACING_MEDIUM
        )
        self._title = QLabel(caption, self._header)
        self._title.setObjectName("playerTitle")
        self._title.setWordWrap(False)
        self._title.installEventFilter(self)
        header_layout.addWidget(self._title, 1)
        root.addWidget(self._header)

        self._stage = QWidget(self)
        self._stage.setObjectName("videoStage")
        self._stage.setMouseTracking(True)
        root.addWidget(self._stage, 1)

        # The video surface and the chrome are laid out, never overlapped. A native
        # window stacked over the mpv surface paints opaque and hides the video, so
        # the controls get their own band and the video takes the rest.
        stage_layout = QVBoxLayout(self._stage)
        stage_layout.setContentsMargins(0, 0, 0, 0)
        stage_layout.setSpacing(0)

        self._video = QWidget(self._stage)
        self._video.setObjectName("videoSurface")
        self._video.setAttribute(Qt.WidgetAttribute.WA_NativeWindow, True)
        self._video.setAttribute(Qt.WidgetAttribute.WA_DontCreateNativeAncestors, False)
        self._video.setMouseTracking(True)
        stage_layout.addWidget(self._video, 1)

        self._controls = PlayerControlsWidget(self._stage)
        self._controls.set_qualities(self._qualities, initial_height)
        stage_layout.addWidget(self._controls, 0)

        # The overlay is an event filter and state presenter only. It must never be
        # given the stage rect: any widget spanning the video occludes the mpv surface.
        self._overlay = PlayerOverlayWidget(self._controls, self._stage)
        self._overlay.set_mouse_sources(self._stage, self._video)
        self._overlay.hide()

        # The status card and the toast are small opaque panels that do float over
        # the video, so each earns a small native handle. What must never happen is a
        # full-stage native window over the video: it paints opaque and hides it.
        for panel in (
            self._overlay.status_card,
            self._overlay.toast,
            self._overlay.stream_chip,
        ):
            panel.setParent(self._stage)
            panel.setAttribute(Qt.WidgetAttribute.WA_NativeWindow, True)
            panel.raise_()
        self._overlay.toastShown.connect(self._layout_panels)
        self._overlay.panelsChanged.connect(self._layout_panels)

        # Compatibility aliases for code that used to inspect the dialog's UI.
        self._chrome = self._controls
        self._timeline = self._controls.timeline
        self._time_label = self._controls.time_label
        self._pause_btn = self._controls.play_button
        self._back_btn = self._controls.rewind_button
        self._fwd_btn = self._controls.forward_button
        self._mute_btn = self._controls.mute_button
        self._vol_slider = self._controls.volume_slider
        self._speed_combo = self._controls.speed_combo
        self._quality_combo = self._controls.quality_combo

        self._controller = PlayerController(
            url,
            self._qualities,
            initial_height,
            self,
        )
        self._controller.stateChanged.connect(self._on_state_changed)
        self._controller.qualitySwitching.connect(self._overlay.show_quality_notice)
        self._controller.seekFeedback.connect(self._overlay.show_seek_feedback)
        self._controller.seekFeedback.connect(self._show_controls)
        self._fullscreen = FullscreenManager(self)
        self._fullscreen.fullscreenChanged.connect(self._on_fullscreen_changed)
        self._connect_controls()
        self._overlay.videoClicked.connect(self._controller.toggle_play_pause)
        self._overlay.videoDoubleClicked.connect(self._fullscreen.toggle)
        self._overlay.retryRequested.connect(self._retry_playback)
        self._overlay.detailsRequested.connect(self._show_error_details)

        self._install_shortcuts()
        self._on_state_changed(self._controller.state)
        self._sync_stage_geometry()

    @property
    def controller(self) -> PlayerController:
        return self._controller

    @property
    def status_text(self) -> str:
        return self._overlay.status_title.text()

    @staticmethod
    def format_time(seconds: float) -> str:
        return format_time(seconds)

    def _connect_controls(self) -> None:
        controls = self._controls
        controller = self._controller
        controls.playPauseRequested.connect(controller.toggle_play_pause)
        controls.rewindRequested.connect(lambda: controller.rewind(10))
        controls.forwardRequested.connect(lambda: controller.forward(10))
        controls.seekStarted.connect(controller.begin_seek)
        controls.seekPreviewChanged.connect(controller.preview_seek)
        controls.seekFinished.connect(controller.finish_seek)
        controls.volumeChanged.connect(controller.set_volume)
        controls.muteRequested.connect(controller.toggle_mute)
        controls.speedChanged.connect(controller.set_speed)
        controls.qualityChanged.connect(controller.set_quality)
        controls.fullscreenRequested.connect(self._fullscreen.toggle)

    def _install_shortcuts(self) -> None:
        shortcuts = (
            ("Space", self._toggle_play_pause),
            ("Left", lambda: self._controller.rewind(10)),
            ("Right", lambda: self._controller.forward(10)),
            ("F", self._fullscreen.toggle),
            ("M", self._controller.toggle_mute),
            ("Up", lambda: self._change_volume(5)),
            ("Down", lambda: self._change_volume(-5)),
            ("Esc", self._exit_fullscreen),
        )
        self._shortcuts: list[QShortcut] = []
        for key, callback in shortcuts:
            shortcut = QShortcut(QKeySequence(key), self)
            shortcut.setContext(Qt.ShortcutContext.WindowShortcut)
            shortcut.activated.connect(lambda command=callback: self._handle_shortcut(command))
            self._shortcuts.append(shortcut)

    def _handle_shortcut(self, command: Callable[[], None]) -> None:
        self._overlay.show_controls()
        command()

    def _toggle_play_pause(self) -> None:
        focus = self.focusWidget()
        if isinstance(focus, QAbstractButton):
            return
        self._controller.toggle_play_pause()

    def _change_volume(self, delta: int) -> None:
        self._controller.set_volume(self._controller.state.volume + delta)

    def _exit_fullscreen(self) -> None:
        if self._fullscreen.is_fullscreen:
            self._fullscreen.exit()

    def start_player(self, runtime_dir: Path | None = None) -> None:
        if self._closed:
            return
        if self._controller.adapter is not None:
            return
        self._runtime_dir = Path(runtime_dir) if runtime_dir is not None else None
        try:
            adapter = MPVAdapter(self._video, mpv_path=self._runtime_dir)
            self._controller.attach_adapter(adapter)
            self._controller.load()
        except Exception as exc:
            log.exception("player startup failed")
            self._controller.fail(str(exc))

    def show_error(self, message: str) -> None:
        self._controller.fail(message)

    def apply_refreshed_streams(
        self,
        qualities: dict[int, str],
        height: int | None = None,
    ) -> None:
        """Reload from a re-extracted quality map after a failed stream load."""
        if self._closed or not qualities:
            return
        target = height if height in qualities else max(qualities)
        self._qualities = dict(qualities)
        self._controls.set_qualities(self._qualities, target)
        self._overlay.show_toast("Посилання на потік прострочене — оновлюю")
        self._controller.replace_stream(self._qualities[target], self._qualities)

    def _retry_playback(self) -> None:
        if self._controller.adapter is None:
            self.runtimeRequested.emit()
        else:
            self._controller.retry()

    def _show_error_details(self) -> None:
        details = self._overlay.details_text().strip()
        box = QMessageBox(self)
        box.setIcon(QMessageBox.Icon.Warning)
        box.setWindowTitle("Деталі відтворення")
        box.setText("Плеєр не зміг відкрити цей потік.")
        box.setDetailedText(details or "Технічних деталей немає.")
        box.exec()

    def _on_state_changed(self, state: PlaybackState) -> None:
        if self._closed:
            return
        self._overlay.set_playback_state(state)
        self._layout_panels()
        if state.status is PlaybackStatus.ERROR and not self._refresh_attempted:
            self._refresh_attempted = True
            self.playbackFailed.emit()

    def _on_fullscreen_changed(self, fullscreen: bool) -> None:
        self._header.setVisible(not fullscreen)
        self._controls.set_fullscreen(fullscreen)
        self._overlay.set_fullscreen(fullscreen)
        self._sync_stage_geometry()
        QTimer.singleShot(0, self._sync_stage_geometry)

    def _show_controls(self) -> None:
        self._overlay.show_controls()

    def show(self) -> None:
        # Open in fullscreen for the focused player experience, while the
        # manager retains the dialog geometry for reliable restoration.
        super().show()
        if not self._showing and not self._fullscreen.is_fullscreen:
            self._showing = True
            try:
                self._fullscreen.enter()
            finally:
                self._showing = False

    def changeEvent(self, event: QEvent) -> None:
        super().changeEvent(event)
        if event.type() == QEvent.Type.WindowStateChange:
            fullscreen = self.isFullScreen()
            self._header.setVisible(not fullscreen)
            self._controls.set_fullscreen(fullscreen)
            self._overlay.set_fullscreen(fullscreen)
            self._sync_stage_geometry()
            QTimer.singleShot(0, self._sync_stage_geometry)

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:
        # Eliding here rather than in resizeEvent: the layout has not yet given the
        # label its new width when the dialog resizes, and a wrapped header would
        # steal height from the video.
        if watched is self._title and event.type() == QEvent.Type.Resize:
            metrics = self._title.fontMetrics()
            self._title.setText(
                metrics.elidedText(self._caption, Qt.TextElideMode.ElideRight, self._title.width())
            )
        return super().eventFilter(watched, event)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._sync_stage_geometry()

    def _layout_panels(self) -> None:
        area = self._stage.rect()

        def place(widget: QWidget, top: int) -> None:
            hint = widget.sizeHint()
            width = min(area.width() - 2 * SPACING_LARGE, max(hint.width(), 1))
            height = max(hint.height(), 1)
            widget.setGeometry(area.x() + (area.width() - width) // 2, top, width, height)

        if self._overlay.status_card.isVisible():
            place(self._overlay.status_card, area.y() + max(SPACING_LARGE, area.height() // 4))
        if self._overlay.stream_chip.isVisible():
            chip = self._overlay.stream_chip
            chip_hint = chip.sizeHint()
            chip.setGeometry(
                area.x() + SPACING_LARGE,
                area.y() + SPACING_LARGE,
                chip_hint.width(),
                chip_hint.height(),
            )
        if self._overlay.toast.isVisible():
            # The toast used to be centred like the status card, so a quality switch
            # during an error hid the message behind it. It belongs on the control bar.
            bar_top = (
                area.y()
                + area.height()
                - (self._controls.height() if self._controls.isVisible() else 0)
            )
            place(
                self._overlay.toast,
                bar_top - self._overlay.toast.sizeHint().height() - SPACING_MEDIUM,
            )
        for panel in (
            self._overlay.status_card,
            self._overlay.toast,
            self._overlay.stream_chip,
        ):
            if panel.isVisible():
                panel.raise_()

    def _sync_stage_geometry(self) -> None:
        if not hasattr(self, "_stage"):
            return
        self._layout_panels()

    def closeEvent(self, event) -> None:
        self._closed = True
        self._controller.close()
        super().closeEvent(event)
