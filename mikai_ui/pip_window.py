"""A frameless, always-on-top mini player.

Deliberately not a strip of chrome: a header, a caption or a taskbar button
would make it a second window pretending to be a float. It is a video plus a
thin play/pause bar.

The window does not move by holding the picture. A click in a surface that is
almost entirely video means "I am watching this", not "I am dragging it", and
a mis-aimed grab that flings the float across the screen is worse than a float
that stays where it was put.
"""

from __future__ import annotations

from PySide6.QtCore import (
    QEasingCurve,
    QEvent,
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
    QVBoxLayout,
    QWidget,
)

from mikai_ui.theme.metrics import ICON_SIZE, PLAYER_FIELD_HEIGHT, SPACING_SMALL
from mikai_ui.theme.widgets import make_icon

SEEK_STEP_SECONDS = 10
CORNER_RADIUS_PX = 16
# theme.metrics.ANIMATION_DURATION_MS is gone, and no shared token would fit
# anyway: this is the only animation the float owns, and the retire delay is a
# rule about the hover over an unfocused window, not a decoration other
# surfaces should inherit
BAR_FADE_MS = 160
BAR_REVEAL_HOLD_MS = 1600


class PipWindow(QWidget):
    """Frameless float holding the video and a minimal play/pause bar."""

    togglePlayRequested = Signal()
    seekRequested = Signal(int)
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

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        self._video_holder = QWidget(self)
        self._video_holder.setObjectName("pipVideo")
        self._video_holder.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        # without tracking, a move over the picture stops at the surface and the
        # float never hears about the pointer hovering it
        self._video_holder.setMouseTracking(True)
        root.addWidget(self._video_holder, 1)

        self._bar = QWidget(self)
        self._bar.setObjectName("pipBar")
        bar_layout = QHBoxLayout(self._bar)
        bar_layout.setContentsMargins(SPACING_SMALL, SPACING_SMALL, SPACING_SMALL, SPACING_SMALL)
        bar_layout.setSpacing(SPACING_SMALL)

        self._status = QLabel("", self._bar)
        self._status.setObjectName("pipStatus")
        bar_layout.addWidget(self._status, 1)

        self._play_button = QPushButton(self._bar)
        self._play_button.setObjectName("pipPlay")
        self._play_button.setFixedSize(PLAYER_FIELD_HEIGHT, PLAYER_FIELD_HEIGHT)
        self._play_button.setIcon(make_icon("pause", size=ICON_SIZE))
        self._play_button.setToolTip("Пауза (Пробіл)")
        self._play_button.setAccessibleName("Пауза")
        self._play_button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._play_button.clicked.connect(self.togglePlayRequested)
        bar_layout.addWidget(self._play_button)

        self._close_button = QPushButton(self._bar)
        self._close_button.setObjectName("pipClose")
        self._close_button.setFixedSize(PLAYER_FIELD_HEIGHT, PLAYER_FIELD_HEIGHT)
        self._close_button.setIcon(make_icon("close", size=ICON_SIZE))
        self._close_button.setToolTip("Закрити (Esc)")
        self._close_button.setAccessibleName("Закрити")
        self._close_button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._close_button.clicked.connect(self.closeRequested)
        bar_layout.addWidget(self._close_button)

        root.addWidget(self._bar, 0)

        self._video: QWidget | None = None

        # the bar is chrome drawn over a picture, so it belongs to the pointer
        # and to the focus rather than sitting on top of another app forever
        self._bar_opacity = 1.0
        self._bar_effect = QGraphicsOpacityEffect(self._bar)
        self._bar_effect.setOpacity(1.0)
        self._bar.setGraphicsEffect(self._bar_effect)
        self._bar_fade = QVariantAnimation(self)
        self._bar_fade.setDuration(BAR_FADE_MS)
        self._bar_fade.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._bar_fade.valueChanged.connect(self._set_bar_opacity)
        self._bar_hide_timer = QTimer(self)
        self._bar_hide_timer.setSingleShot(True)
        self._bar_hide_timer.timeout.connect(self._retire_bar)
        # the bar's own children swallow moves that never reach the float, so
        # each one has to be watched separately for the reveal
        for watched in (
            self,
            self._video_holder,
            self._bar,
            self._status,
            self._play_button,
            self._close_button,
        ):
            watched.installEventFilter(self)

    # -- content --------------------------------------------------------
    def set_video(self, widget: QWidget) -> None:
        """Adopt the player's video surface and remember it for the way back."""
        self._video = widget
        self._video_holder.installEventFilter(self)
        widget.installEventFilter(self)
        widget.setParent(self._video_holder)
        widget.setMouseTracking(True)
        widget.setGeometry(self._video_holder.rect())
        widget.show()

    def release_video(self) -> QWidget | None:
        video, self._video = self._video, None
        return video

    def set_playing(self, playing: bool) -> None:
        icon = "pause" if playing else "play"
        hint = "Пауза (Пробіл)" if playing else "Продовжити (Пробіл)"
        self._play_button.setIcon(make_icon(icon, size=ICON_SIZE))
        self._play_button.setToolTip(hint)
        self._play_button.setAccessibleName(hint)

    def set_time_text(self, text: str) -> None:
        self._status.setText(text)

    # -- the controls come and go with the focus -----------------------
    def _set_bar_opacity(self, value: float) -> None:
        self._bar_opacity = float(value)
        self._bar_effect.setOpacity(self._bar_opacity)
        self._bar.setVisible(self._bar_opacity > 0.01)

    def _reveal_bar(self) -> None:
        # opacity and visibility are one state, so a float that is already lit
        # costs nothing here however often the pointer moves across it
        if self._bar_opacity < 0.99:
            self._bar_fade.stop()
            self._bar_fade.setStartValue(self._bar_opacity)
            self._bar_fade.setEndValue(1.0)
            self._bar_fade.start()
        # a hover can reach a float the user never activated, so the bar has to
        # retire again on its own rather than wait for a focus event that will
        # never arrive
        self._bar_hide_timer.start(BAR_REVEAL_HOLD_MS)

    def _hide_bar(self) -> None:
        self._bar_hide_timer.stop()
        if self._bar_opacity > 0.01:
            self._bar_fade.stop()
            self._bar_fade.setStartValue(self._bar_opacity)
            self._bar_fade.setEndValue(0.0)
            self._bar_fade.start()

    def _retire_bar(self) -> None:
        if not self.isActiveWindow():
            self._hide_bar()

    def eventFilter(self, watched, event) -> bool:  # noqa: N802 - Qt naming
        """Keep the bar in step with the pointer and with the activation.

        Those are the only two signals that reach a float sitting behind another
        app. A click on the picture deliberately does nothing: in a float that is
        mostly video, a click is not a command.
        """
        kind = event.type()
        mine = watched is self or self.isAncestorOf(watched)
        if mine and kind in (QEvent.Type.MouseMove, QEvent.Type.MouseButtonPress):
            self._reveal_bar()
        if watched is self:
            # Win32 delivers activation to the top level, and a changeEvent
            # override never sees it, so the float watches its own events
            if kind == QEvent.Type.WindowActivate:
                self._reveal_bar()
            elif kind == QEvent.Type.WindowDeactivate:
                self._hide_bar()
        if watched is self._video_holder and kind == QEvent.Type.Resize:
            # the bar shares the layout, so a hidden bar hands its height to the
            # picture -- and the surface is a plain child, it does not follow
            if self._video is not None:
                self._video.setGeometry(self._video_holder.rect())
        return super().eventFilter(watched, event)

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

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt naming
        super().resizeEvent(event)
        self._apply_corner_mask()
        if self._video is not None:
            self._video.setGeometry(self._video_holder.rect())

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802 - Qt naming
        key = event.key()
        if key == Qt.Key.Key_Left:
            self.seekRequested.emit(-SEEK_STEP_SECONDS)
        elif key == Qt.Key.Key_Right:
            self.seekRequested.emit(SEEK_STEP_SECONDS)
        elif key in (Qt.Key.Key_Space,):
            self.togglePlayRequested.emit()
        else:
            super().keyPressEvent(event)
