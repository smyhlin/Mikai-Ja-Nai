"""A frameless, always-on-top mini player.

Deliberately not a strip of chrome: a header, a caption or a taskbar button
would make it a second window pretending to be a float. It is a video plus a
thin play/pause bar, and the window is moved by holding the picture.
"""

from __future__ import annotations

from PySide6.QtCore import QEvent, QPoint, Qt, Signal
from PySide6.QtGui import QKeyEvent, QPainterPath, QRegion
from PySide6.QtWidgets import QHBoxLayout, QLabel, QPushButton, QVBoxLayout, QWidget

from mikai_ui.theme.metrics import ICON_SIZE, PLAYER_FIELD_HEIGHT, SPACING_SMALL
from mikai_ui.theme.widgets import make_icon

# a press only becomes a drag past this many pixels, so a stray click on the
# picture does not nudge the window
DRAG_THRESHOLD_PX = 6
SEEK_STEP_SECONDS = 10
CORNER_RADIUS_PX = 16


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
        self._press_pos: QPoint | None = None
        self._window_pos: QPoint | None = None
        self._dragging = False

    # -- content --------------------------------------------------------
    def set_video(self, widget: QWidget) -> None:
        """Adopt the player's video surface and remember it for the way back."""
        self._video = widget
        self._video_holder.installEventFilter(self)
        widget.installEventFilter(self)
        widget.setParent(self._video_holder)
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

    # -- moving the float ----------------------------------------------
    def _begin_drag(self, pos: QPoint) -> None:
        self._press_pos = pos
        self._window_pos = self.pos()
        self._dragging = False

    def _continue_drag(self, pos: QPoint) -> None:
        if self._press_pos is None:
            return
        delta = pos - self._press_pos
        if not self._dragging and (delta.manhattanLength() > DRAG_THRESHOLD_PX):
            self._dragging = True
        if self._dragging and self._window_pos is not None:
            self.move(self._window_pos + delta)

    def _end_drag(self) -> None:
        self._press_pos = None
        self._window_pos = None
        self._dragging = False

    def eventFilter(self, watched, event) -> bool:  # noqa: N802 - Qt naming
        """Press-and-hold on the picture moves the window; a plain click does not.

        There is deliberately no play/pause on a picture click: in a float that
        is mostly picture, a click means "I am grabbing this", not "toggle".
        """
        if watched is self._video_holder or watched is self._video:
            kind = event.type()
            if kind == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.LeftButton:
                self._begin_drag(event.position().toPoint())
                self.setFocus(Qt.FocusReason.MouseFocusReason)
                return True
            if kind == QEvent.Type.MouseMove and self._press_pos is not None:
                self._continue_drag(event.position().toPoint())
                return True
            if kind == QEvent.Type.MouseButtonRelease:
                self._end_drag()
                return True
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
