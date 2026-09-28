"""Window chrome, flank navigation, source pill, footer and speed popover.

Every widget here is command-only: it reports what the user asked for and never
reaches for the mpv engine, so the chrome can be hidden and shown without the
playback state noticing.
"""

from __future__ import annotations

from PySide6.QtCore import QSize, Qt, Signal
from PySide6.QtGui import QFontMetrics, QPixmap
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSlider,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from mikai_ui.theme.colors import ICON_DISABLED
from mikai_ui.theme.metrics import (
    ICON_SIZE,
    PLAYER_CHEVRON_MARGIN,
    PLAYER_CHEVRON_SIZE,
    PLAYER_CHROME_BUTTON_SIZE,
    SPACING_MEDIUM,
    SPACING_SMALL,
    SPACING_XS,
    SPEED_FLYOUT_OFFSET,
)
from mikai_ui.theme.widgets import make_icon


def _icon_button(
    object_name: str,
    tooltip: str,
    icon_name: str,
    size: int,
) -> QPushButton:
    button = QPushButton()
    button.setObjectName(object_name)
    button.setIcon(make_icon(icon_name, size=size, disabled_color=ICON_DISABLED))
    button.setIconSize(QSize(size, size))
    button.setFixedSize(size, size)
    button.setToolTip(tooltip)
    button.setAccessibleName(tooltip)
    # An overlay button that takes focus swallows the transport's space and
    # arrow keys while the pointer is simply resting on the chrome.
    button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
    return button


class PlayerDismissButton(QWidget):
    """A single close control for the chromeless player, styled like the cover viewer.

    The cover viewer has no window frame and no minimise/expand pair — it is a
    full-screen overlay dismissed by a single action. The player mirrors that:
    one unobtrusive close button, no minimise, no expand. Fullscreen stays on F
    and the dock's own fullscreen control; the three-button frame added both and
    they broke the player (minimise from fullscreen left the window
    simultaneously minimised and fullscreen).
    """

    closeRequested = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("playerDismiss")
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)

        self.close_button = _icon_button(
            "playerDismissButton", "Закрити плеєр", "close", PLAYER_CHROME_BUTTON_SIZE
        )
        self.close_button.clicked.connect(self.closeRequested)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(self.close_button)
        self.setFixedHeight(PLAYER_CHROME_BUTTON_SIZE)


class FlankChevrons(QWidget):
    """Oversized previous/next buttons pinned to the left and right of the video."""

    previousRequested = Signal()
    nextRequested = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("playerChevrons")
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        row = QHBoxLayout(self)
        row.setContentsMargins(PLAYER_CHEVRON_MARGIN, 0, PLAYER_CHEVRON_MARGIN, 0)
        row.setSpacing(0)

        self.previous_button = _icon_button(
            "playerChevron", "Попередня серія", "chevron_left", PLAYER_CHEVRON_SIZE
        )
        self.previous_button.clicked.connect(self.previousRequested)
        self.next_button = _icon_button(
            "playerChevron", "Наступна серія", "chevron_right", PLAYER_CHEVRON_SIZE
        )
        self.next_button.clicked.connect(self.nextRequested)
        row.addWidget(self.previous_button, 0, Qt.AlignmentFlag.AlignVCenter)
        row.addStretch(1)
        row.addWidget(self.next_button, 0, Qt.AlignmentFlag.AlignVCenter)

    def set_available(self, has_previous: bool, has_next: bool) -> None:
        self.previous_button.setEnabled(has_previous)
        self.next_button.setEnabled(has_next)


class SourcePill(QWidget):
    """A compact pill naming the stream's origin, with a link back to it."""

    linkClicked = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("playerSourcePill")
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)

        row = QHBoxLayout(self)
        row.setContentsMargins(SPACING_XS, SPACING_XS, SPACING_SMALL, SPACING_XS)
        row.setSpacing(SPACING_SMALL)

        self.link_button = QPushButton("Джерело")
        self.link_button.setObjectName("playerSourceLink")
        self.link_button.setIcon(make_icon("play", size=ICON_SIZE, disabled_color=ICON_DISABLED))
        self.link_button.setIconSize(QSize(ICON_SIZE, ICON_SIZE))
        self.link_button.setToolTip("Відкрити сторінку джерела")
        self.link_button.setAccessibleName("Відкрити сторінку джерела")
        self.link_button.clicked.connect(self.linkClicked)
        row.addWidget(self.link_button)

        self.detail_label = QLabel("")
        self.detail_label.setObjectName("playerSourceDetail")
        row.addWidget(self.detail_label)

    def set_source(self, label: str, detail: str) -> None:
        self.link_button.setText(label)
        self.detail_label.setText(detail)
        self.link_button.setAccessibleName(f"Відкрити джерело: {label}")


class PlayerFooter(QWidget):
    """A transparent strip: what is playing on the left, actions on the right."""

    openRequested = Signal()
    moreRequested = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("playerFooter")
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self._name = ""
        self._origin = ""

        row = QHBoxLayout(self)
        row.setContentsMargins(SPACING_MEDIUM, 0, SPACING_MEDIUM, 0)
        row.setSpacing(SPACING_SMALL)

        self.name_label = QLabel("")
        self.name_label.setObjectName("playerFooterName")
        self.name_label.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        self.origin_label = QLabel("")
        self.origin_label.setObjectName("playerFooterOrigin")
        self.origin_label.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        row.addWidget(self.name_label, 1)
        row.addWidget(self.origin_label)
        row.addStretch(0)

        self.open_button = _icon_button(
            "playerFooterButton", "Відкрити сторінку серії", "play", PLAYER_CHROME_BUTTON_SIZE
        )
        self.open_button.clicked.connect(self.openRequested)
        self.more_button = _icon_button(
            "playerFooterButton", "Більше дій", "gear", PLAYER_CHROME_BUTTON_SIZE
        )
        self.more_button.clicked.connect(self.moreRequested)
        row.addWidget(self.open_button)
        row.addWidget(self.more_button)
        self.setFixedHeight(PLAYER_CHROME_BUTTON_SIZE)

    def set_metadata(self, name: str, origin: str) -> None:
        self._name = name
        self._origin = origin
        self._elide()

    def _elide(self) -> None:
        """Clip the metadata instead of letting it shove the actions off-screen."""
        metrics_name = QFontMetrics(self.name_label.font())
        metrics_origin = QFontMetrics(self.origin_label.font())
        chrome = (
            self.open_button.sizeHint().width()
            + self.more_button.sizeHint().width()
            + SPACING_SMALL
            + 2 * SPACING_MEDIUM
        )
        available = self.width() - chrome
        if available <= 0:
            # Before the first layout pass there is no width to elide against, and
            # clipping to it would blank the labels outright.
            self.name_label.setText(self._name)
            self.origin_label.setText(self._origin)
            return
        origin_width = min(metrics_origin.horizontalAdvance(self._origin), available // 2)
        self.name_label.setText(
            metrics_name.elidedText(
                self._name,
                Qt.TextElideMode.ElideRight,
                max(0, available - origin_width - SPACING_SMALL),
            )
        )
        self.origin_label.setText(
            metrics_origin.elidedText(self._origin, Qt.TextElideMode.ElideRight, origin_width)
        )

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt naming
        super().resizeEvent(event)
        self._elide()


class SpeedFlyout(QWidget):
    """A popover over a continuous speed slider plus named presets."""

    speedChanged = Signal(float)

    SPEED_MIN = 0.25
    SPEED_MAX = 4.0
    SCALE = 100
    PRESETS = (
        (0.5, "0.5 Повільна"),
        (1.0, "1.0 Звичайна"),
        (1.2, "1.2 Пришвидшена"),
        (1.5, "1.5 Швидка"),
        (1.7, "1.7 Дуже швидка"),
        (2.0, "2.0 Подвійна"),
    )
    PRESET_TOLERANCE = 0.05

    def __init__(self, parent: QWidget | None = None) -> None:
        # A popover that opens over the picture has to be its own window. A
        # child of the control bar would be painted into the stage backing
        # store, which sits UNDER the native mpv surface, so the video would
        # simply cover it.
        super().__init__(None, Qt.WindowType.Popup)
        self.setObjectName("playerSpeedFlyout")
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setWindowFlags(Qt.WindowType.Popup)
        self._rows: list[tuple[float, QPushButton, QLabel]] = []

        column = QVBoxLayout(self)
        column.setContentsMargins(SPACING_MEDIUM, SPACING_MEDIUM, SPACING_MEDIUM, SPACING_MEDIUM)
        column.setSpacing(SPACING_SMALL)

        header = QHBoxLayout()
        header.setSpacing(SPACING_SMALL)
        title = QLabel("Швидкість")
        title.setObjectName("muted")
        header.addWidget(title)
        header.addStretch(1)
        self.value_label = QLabel("1.0x")
        self.value_label.setObjectName("playerSpeedValue")
        self.value_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        header.addWidget(self.value_label)
        column.addLayout(header)

        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setObjectName("playerSpeedSlider")
        self.slider.setRange(round(self.SPEED_MIN * self.SCALE), round(self.SPEED_MAX * self.SCALE))
        self.slider.setValue(self.SCALE)
        self.slider.setAccessibleName("Швидкість відтворення")
        self.slider.setToolTip("Швидкість відтворення")
        self.slider.valueChanged.connect(self._slider_moved)
        column.addWidget(self.slider)

        for speed, label in self.PRESETS:
            # QPushButton paints its icon before the text, so a trailing check has
            # to be a sibling label rather than setIcon on the row itself.
            line = QHBoxLayout()
            line.setContentsMargins(0, 0, 0, 0)
            line.setSpacing(SPACING_SMALL)
            row = QPushButton(label)
            row.setObjectName("playerSpeedPreset")
            row.setAccessibleName(f"Швидкість {label}")
            row.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            row.clicked.connect(lambda _checked=False, value=speed: self._preset_picked(value))
            check = QLabel(
                pixmap=make_icon("chevron_right", size=ICON_SIZE).pixmap(ICON_SIZE, ICON_SIZE)
            )
            check.setFixedWidth(ICON_SIZE)
            line.addWidget(row, 1)
            line.addWidget(check, 0, Qt.AlignmentFlag.AlignVCenter)
            column.addLayout(line)
            self._rows.append((speed, row, check))

        self.set_speed(1.0)

    def _slider_moved(self, value: int) -> None:
        speed = round(value / self.SCALE, 2)
        self.value_label.setText(f"{speed:g}x")
        self._select(speed)
        self.speedChanged.emit(speed)

    def _preset_picked(self, speed: float) -> None:
        self.set_speed(speed)
        self.speedChanged.emit(speed)

    def set_speed(self, speed: float) -> None:
        value = max(self.SPEED_MIN, min(self.SPEED_MAX, float(speed)))
        self.slider.blockSignals(True)
        self.slider.setValue(round(value * self.SCALE))
        self.slider.blockSignals(False)
        self.value_label.setText(f"{value:g}x")
        self._select(value)

    def _select(self, speed: float) -> None:
        chosen = next(
            (preset for preset, _, _ in self._rows if abs(preset - speed) <= self.PRESET_TOLERANCE),
            None,
        )
        for preset, row, check in self._rows:
            active = preset == chosen
            name = "playerSpeedPresetActive" if active else "playerSpeedPreset"
            if row.objectName() != name:
                row.setObjectName(name)
                # Qt resolves object-name rules once, so a renamed row keeps the
                # old styling until the style is repolished.
                row.style().unpolish(row)
                row.style().polish(row)
            check.setPixmap(
                make_icon("chevron_right", size=ICON_SIZE).pixmap(ICON_SIZE, ICON_SIZE)
                if active
                else QPixmap()
            )

    def popup_for(self, button: QPushButton) -> None:
        """Park above the trigger, right edges aligned.

        It is a top-level popup, so the anchor is taken in screen coordinates
        and the popover closes itself when the user clicks anywhere else.
        """
        anchor = button.mapToGlobal(button.rect().topRight())
        self.adjustSize()
        self.move(anchor.x() - self.width(), anchor.y() - self.height() - SPEED_FLYOUT_OFFSET)
        self.show()
        self.raise_()

    def dismiss(self) -> None:
        self.hide()
