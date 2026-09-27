"""Fullscreen-aware control and playback-status overlay."""

from __future__ import annotations

from PySide6.QtCore import QEvent, QPropertyAnimation, Qt, QTimer, Signal
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QProgressBar,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from mikai_ui.player_controller import PlaybackState, PlaybackStatus
from mikai_ui.player_controls import PlayerControlsWidget
from mikai_ui.theme.metrics import (
    ANIMATION_DURATION_MS,
    AUTO_HIDE_MS,
    PLAYER_BAR_MIN_HEIGHT,
    SINGLE_CLICK_DELAY_MS,
    SPACING_LARGE,
    SPACING_SMALL,
    SPINNER_HEIGHT,
    SPINNER_WIDTH,
    STATUS_CARD_MIN_WIDTH,
    TOAST_DURATION_MS,
)


class PlayerOverlayWidget(QWidget):
    """Show adaptive status feedback and hide controls after fullscreen idle."""

    retryRequested = Signal()
    detailsRequested = Signal()
    videoClicked = Signal()
    videoDoubleClicked = Signal()
    mouseActivity = Signal()
    toastShown = Signal()

    def __init__(
        self,
        controls: PlayerControlsWidget,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.controls = controls
        self._fullscreen = False
        self._technical_details = ""
        self._click_timer = QTimer(self)
        self._click_timer.setSingleShot(True)
        self._click_timer.timeout.connect(self.videoClicked)

        self.setObjectName("playerOverlay")
        # The overlay is a non-native canvas and must stay that way. A full-stage
        # native window is opaque and hides the mpv surface; a layered one (the only
        # way to get translucency) composites as black over it. The dialog parents
        # the controls and the toast to the stage instead, so nothing here forces
        # this widget to become native.
        self.setMouseTracking(True)

        # No layout here on purpose: this widget holds logic and event filters only.
        # The status card and toast are reparented to the stage by the dialog, which
        # owns their placement, because a native child of this widget would drag the
        # whole overlay native and hide the video again.
        self.status_card = QWidget(self)
        self.status_card.setObjectName("playerStatusCard")
        self.status_card.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.status_card.setMinimumWidth(STATUS_CARD_MIN_WIDTH)
        status_layout = QVBoxLayout(self.status_card)
        status_layout.setContentsMargins(SPACING_LARGE, SPACING_LARGE, SPACING_LARGE, SPACING_LARGE)
        status_layout.setSpacing(SPACING_SMALL)
        self.status_title = QLabel("Відео завантажується…", self.status_card)
        self.status_title.setObjectName("playerStatusTitle")
        self.status_title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        status_layout.addWidget(self.status_title)
        self.status_message = QLabel("", self.status_card)
        self.status_message.setObjectName("muted")
        self.status_message.setWordWrap(True)
        self.status_message.setAlignment(Qt.AlignmentFlag.AlignCenter)
        status_layout.addWidget(self.status_message)
        self.spinner = QProgressBar(self.status_card)
        self.spinner.setObjectName("playerSpinner")
        self.spinner.setRange(0, 0)
        self.spinner.setTextVisible(False)
        self.spinner.setFixedSize(SPINNER_WIDTH, SPINNER_HEIGHT)
        spinner_row = QHBoxLayout()
        spinner_row.addStretch(1)
        spinner_row.addWidget(self.spinner)
        spinner_row.addStretch(1)
        status_layout.addLayout(spinner_row)
        status_actions = QHBoxLayout()
        status_actions.addStretch(1)
        self.retry_button = QPushButton("Повторити", self.status_card)
        self.retry_button.setObjectName("primary")
        self.retry_button.setAccessibleName("Повторити відтворення")
        self.retry_button.clicked.connect(self.retryRequested)
        self.details_button = QPushButton("Деталі", self.status_card)
        self.details_button.setObjectName("playerGhost")
        self.details_button.setAccessibleName("Показати технічні деталі")
        self.details_button.clicked.connect(self.detailsRequested)
        status_actions.addWidget(self.retry_button)
        status_actions.addWidget(self.details_button)
        status_actions.addStretch(1)
        status_layout.addLayout(status_actions)
        self.status_card.hide()
        self.status_card.setMouseTracking(True)
        self.status_card.installEventFilter(self)
        for child in self.status_card.findChildren(QWidget):
            child.setMouseTracking(True)
            child.installEventFilter(self)

        self.toast = QLabel("", self)
        self.toast.setObjectName("playerToast")
        self.toast.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.toast.hide()
        self.toast.setMouseTracking(True)
        self.toast.installEventFilter(self)
        self._controls_height = max(PLAYER_BAR_MIN_HEIGHT, self.controls.sizeHint().height())
        self._animation = QPropertyAnimation(self.controls, b"maximumHeight", self)
        self._animation.setDuration(ANIMATION_DURATION_MS)
        self._animation.finished.connect(self._finish_hide_once)
        self.controls.installEventFilter(self)
        for child in self.controls.findChildren(QWidget):
            child.setMouseTracking(True)
            child.installEventFilter(self)

        self._hide_timer = QTimer(self)
        self._hide_timer.setSingleShot(True)
        self._hide_timer.setInterval(AUTO_HIDE_MS)
        self._hide_timer.timeout.connect(self.hide_controls)
        self._toast_timer = QTimer(self)
        self._toast_timer.setSingleShot(True)
        self._toast_timer.setInterval(TOAST_DURATION_MS)
        self._toast_timer.timeout.connect(self.toast.hide)

    def set_mouse_sources(self, *widgets: QWidget) -> None:
        for widget in widgets:
            widget.setMouseTracking(True)
            widget.installEventFilter(self)

    def set_playback_state(self, state: PlaybackState) -> None:
        self.controls.set_playback_state(state)
        self._technical_details = state.error or ""
        status = state.status
        if status is PlaybackStatus.LOADING:
            title, message = "Відео завантажується…", ""
            visible, spinner = True, True
        elif status is PlaybackStatus.BUFFERING:
            title, message = "Буферизація…", "Відтворення продовжиться автоматично."
            visible, spinner = True, True
        elif status is PlaybackStatus.ERROR:
            title = "Відео недоступне"
            message = "Можливі причини: проблема з мережею, недійсний потік або відсутній кодек."
            visible, spinner = True, False
        elif status is PlaybackStatus.FINISHED:
            title, message = "Відтворення завершено", "Натисніть «Пуск», щоб переглянути ще раз."
            visible, spinner = True, False
        else:
            visible, spinner = False, False
            title, message = "", ""
        self.status_title.setText(title)
        self.status_message.setText(message)
        self.spinner.setVisible(spinner)
        self.retry_button.setVisible(status in (PlaybackStatus.ERROR, PlaybackStatus.FINISHED))
        self.retry_button.setText("Ще раз" if status is PlaybackStatus.FINISHED else "Повторити")
        self.details_button.setVisible(status is PlaybackStatus.ERROR)
        self.status_card.setVisible(visible)

    def show_quality_notice(self, height: int) -> None:
        self.show_toast(f"Якість: {height}p")

    def show_seek_feedback(self, text: str) -> None:
        self.show_toast(text)

    def show_toast(self, text: str) -> None:
        self.toast.setText(text)
        self.toast.show()
        self.toastShown.emit()
        self._toast_timer.start()

    def set_fullscreen(self, fullscreen: bool) -> None:
        self._fullscreen = bool(fullscreen)
        if self._fullscreen:
            self.show_controls()
            self._schedule_hide()
        else:
            self._hide_timer.stop()
            self.show_controls()

    def _schedule_hide(self) -> None:
        if self._fullscreen:
            self._hide_timer.start()

    def show_controls(self) -> None:
        self.mouseActivity.emit()
        self._hide_timer.stop()
        was_hidden = not self.controls.isVisible()
        if was_hidden:
            self.controls.setMaximumHeight(0)
            self.controls.show()
        self.controls.setEnabled(True)
        self._animation.stop()
        self._animation.setStartValue(0 if was_hidden else self.controls.height())
        self._animation.setEndValue(self._controls_height)
        if self._fullscreen:
            self._animation.start()
        else:
            self.controls.setMaximumHeight(self._controls_height)
        self._schedule_hide()
        self.unsetCursor()

    def hide_controls(self) -> None:
        if not self._fullscreen:
            return
        self._animation.stop()
        self._animation.setStartValue(self.controls.height())
        self._animation.setEndValue(0)
        self._animation.start()
        self.setCursor(Qt.CursorShape.BlankCursor)

    def _finish_hide_once(self) -> None:
        if self.controls.maximumHeight() <= 0 and self._fullscreen:
            self.controls.hide()
            self.controls.setEnabled(False)

    def eventFilter(self, watched: QWidget, event: QEvent) -> bool:
        kind = event.type()
        in_status = (
            watched is self.status_card
            or self.status_card.isAncestorOf(watched)
            or watched is self.toast
        )
        if kind == QEvent.Type.MouseMove:
            self.show_controls()
            return False
        if in_status and kind in (
            QEvent.Type.MouseButtonPress,
            QEvent.Type.MouseButtonDblClick,
        ):
            self.show_controls()
            return False
        elif (
            kind == QEvent.Type.MouseButtonPress
            and watched is not self.controls
            and not self.controls.isAncestorOf(watched)
        ):
            self.show_controls()
            if self._click_timer.isActive():
                self._click_timer.stop()
                self.videoDoubleClicked.emit()
            else:
                self._click_timer.start(SINGLE_CLICK_DELAY_MS)
            event.accept()
            return True
        elif (
            kind == QEvent.Type.MouseButtonDblClick
            and watched is not self.controls
            and not self.controls.isAncestorOf(watched)
        ):
            self.show_controls()
            self._click_timer.stop()
            self.videoDoubleClicked.emit()
            event.accept()
            return True
        return False

    def mouseMoveEvent(self, event) -> None:
        self.show_controls()
        super().mouseMoveEvent(event)

    def mousePressEvent(self, event) -> None:
        if self.childAt(event.position().toPoint()) is None:
            self.show_controls()
            if self._click_timer.isActive():
                self._click_timer.stop()
                self.videoDoubleClicked.emit()
            else:
                self._click_timer.start(SINGLE_CLICK_DELAY_MS)
        event.accept()

    def mouseDoubleClickEvent(self, event) -> None:
        self._click_timer.stop()
        self.videoDoubleClicked.emit()
        event.accept()

    def details_text(self) -> str:
        return self._technical_details
