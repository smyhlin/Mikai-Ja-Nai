"""A non-scrolling, content-sized quality picker for the player bar.

A stock QComboBox popup is a scroll view: it caps its height, grows a scrollbar
and puts the current item under the pointer's heels. This is a frameless popup
that sizes to its rows instead, so every quality is visible at once and the
panel never scrolls.
"""

from __future__ import annotations

from PySide6.QtCore import QPoint, Qt, Signal
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import QPushButton, QVBoxLayout, QWidget

from mikai_ui.theme.metrics import (
    PLAYER_FIELD_HEIGHT,
    PLAYER_FIELD_MIN_WIDTH,
    PLAYER_FIELD_ROW_HEIGHT,
)

# Familiar tier names; heights are what the stream actually offers, the tier is
# only a reading aid, so an unknown height simply shows no tier.
QUALITY_TIERS: tuple[tuple[int, str], ...] = (
    (2160, "4K"),
    (1440, "2K"),
    (1080, "Full HD"),
    (720, "HD"),
    (480, "SD"),
    (360, "низька"),
)


def quality_tier(height: int) -> str:
    for minimum, name in QUALITY_TIERS:
        if height >= minimum:
            return name
    return ""


class QualityPicker(QPushButton):
    """Button that opens a frameless, non-scrolling list of quality heights."""

    qualitySelected = Signal(int)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("qualityPicker")
        self.setMinimumWidth(PLAYER_FIELD_MIN_WIDTH)
        # the chip owns its own height: the stylesheet only decorates it, and a
        # widget that relies on QSS for geometry collapses wherever it is not loaded
        self.setFixedHeight(PLAYER_FIELD_HEIGHT)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setAccessibleName("Якість відео")
        self.setToolTip("Якість відео")
        self.clicked.connect(self._toggle)
        self._heights: list[int] = []
        self._selected: int | None = None
        self._panel: QWidget | None = None

    # -- content ---------------------------------------------------------
    def set_qualities(self, qualities: dict[int, str], selected: int | None) -> None:
        self._heights = sorted(qualities, reverse=True)
        self._selected = (
            selected if selected in self._heights else (self._heights[0] if self._heights else None)
        )
        self._refresh_label()
        self.setVisible(len(self._heights) > 1)
        self._dismiss()

    def heights(self) -> list[int]:
        return list(self._heights)

    def selected_height(self) -> int | None:
        return self._selected

    def currentText(self) -> str:
        return f"{self._selected}p" if self._selected is not None else ""

    def count(self) -> int:
        return len(self._heights)

    def itemText(self, index: int) -> str:
        return f"{self._heights[index]}p"

    def currentIndex(self) -> int:
        if self._selected is None:
            return -1
        return self._heights.index(self._selected)

    def setCurrentIndex(self, index: int) -> None:
        if 0 <= index < len(self._heights):
            self._choose(self._heights[index])

    def adopt_external(self, height: int) -> None:
        """Mirror a height the stream picked itself, without re-emitting.

        A selection through the panel is a user action and must reach the
        engine; a fallback inside the stream is not, and echoing it back would
        bounce between the two.
        """
        if height in self._heights and height != self._selected:
            self._selected = height
            self._refresh_label()

    # -- panel -----------------------------------------------------------
    def _refresh_label(self) -> None:
        text = self.currentText()
        if self._selected is not None and quality_tier(self._selected):
            text = f"{text} · {quality_tier(self._selected)}"
        self.setText(text)

    def _toggle(self) -> None:
        if self._panel is not None:
            self._dismiss()
        elif self._heights:
            self._present()

    def _present(self) -> None:
        panel = QWidget(self.window(), Qt.WindowType.Popup)
        panel.setObjectName("qualityPanel")
        rows = QVBoxLayout(panel)
        rows.setContentsMargins(4, 4, 4, 4)
        rows.setSpacing(2)
        for height in self._heights:
            rows.addWidget(self._row(panel, height))
        panel.adjustSize()
        origin = self.mapToGlobal(QPoint(0, self.height() + 2))
        screen = self.screen().availableGeometry()
        x = min(origin.x(), screen.right() - panel.width())
        y = origin.y()
        if y + panel.height() > screen.bottom():
            # no room below: open upward rather than adding a scrollbar
            y = max(screen.top(), self.mapToGlobal(QPoint(0, 0)).y() - panel.height() - 2)
        panel.move(x, y)
        panel.show()
        self._panel = panel

    def _row(self, panel: QWidget, height: int) -> QPushButton:
        row = QPushButton(_row_label(height), panel)
        row.setObjectName("qualityRowCurrent" if height == self._selected else "qualityRow")
        row.setCursor(Qt.CursorShape.PointingHandCursor)
        row.setFixedHeight(PLAYER_FIELD_ROW_HEIGHT)
        row.setMinimumWidth(self.width() - 8)
        row.clicked.connect(lambda _checked=False, value=height: self._choose(value))
        return row

    def _choose(self, height: int) -> None:
        self._dismiss()
        if height == self._selected:
            return
        self._selected = height
        self._refresh_label()
        self.qualitySelected.emit(height)

    def _dismiss(self) -> None:
        panel, self._panel = self._panel, None
        if panel is not None:
            panel.deleteLater()

    # -- keyboard --------------------------------------------------------
    def keyPressEvent(self, event: QKeyEvent) -> None:
        key = event.key()
        if key in (Qt.Key.Key_Escape, Qt.Key.Key_Return, Qt.Key.Key_Enter):
            self._dismiss()
            return
        if key in (Qt.Key.Key_Down, Qt.Key.Key_Up):
            step = 1 if key == Qt.Key.Key_Down else -1
            if len(self._heights) > 1:
                current = self.currentIndex()
                self._choose(self._heights[(current + step) % len(self._heights)])
            return
        super().keyPressEvent(event)

    def focusOutEvent(self, event) -> None:
        self._dismiss()
        super().focusOutEvent(event)

    def hideEvent(self, event) -> None:
        self._dismiss()
        super().hideEvent(event)


def _row_label(height: int) -> str:
    tier = quality_tier(height)
    return f"{height}p · {tier}" if tier else f"{height}p"
