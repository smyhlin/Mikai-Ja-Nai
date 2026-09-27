"""Range-chip pager shared by the search results and a release's episode grid.

A chip is one page and is labelled with the entries that page holds, so the
numbers always match the list or grid above. A window of PAGER_CHIP_WINDOW chips
follows the current page, the arrows step one page, and the two extra arrows and
the page box jump straight to a page.

The page count belongs to the caller: the server answers `pages` for its own page
size, so it has to be computed from the total and the page size before it gets
here. One-based everywhere, as displayed.
"""

from __future__ import annotations

from PySide6.QtCore import Signal
from PySide6.QtWidgets import QHBoxLayout, QLabel, QPushButton, QSpinBox, QWidget

from mikai_ui.theme.metrics import PAGER_CHIP_WINDOW, SPACING_SMALL


class RangePager(QWidget):
    """`⏮ ←  [chips]  → ⏭` plus a page box. Emits the requested page, 1-based."""

    pageRequested = Signal(int)

    def __init__(self, page_size: int, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._page_size = max(1, page_size)
        self._page = 1
        self._pages = 1
        self._entries = 0
        self._numbers: list[str] | None = None
        self._syncing = False

        row = QHBoxLayout(self)
        row.setContentsMargins(8, 4, 8, 4)
        row.setSpacing(SPACING_SMALL)
        self.first_btn = self._arrow("⏮", "На першу сторінку")
        self.prev_btn = self._arrow("←", "Попередня сторінка")
        self.chips_host = QWidget()
        self._chip_layout = QHBoxLayout(self.chips_host)
        self._chip_layout.setContentsMargins(0, 0, 0, 0)
        self._chip_layout.setSpacing(SPACING_SMALL)
        self.next_btn = self._arrow("→", "Наступна сторінка")
        self.last_btn = self._arrow("⏭", "На останню сторінку")
        self.jump_label = QLabel("")
        self.jump_label.setObjectName("muted")
        self.jump = QSpinBox()
        self.jump.setRange(1, 1)
        self.jump.setToolTip("Перейти на сторінку")
        self.jump.setAccessibleName("Номер сторінки")
        self.jump.valueChanged.connect(self._on_jump)

        self.first_btn.clicked.connect(lambda _checked=False: self._emit(1))
        self.prev_btn.clicked.connect(lambda _checked=False: self._step(-1))
        self.next_btn.clicked.connect(lambda _checked=False: self._step(1))
        self.last_btn.clicked.connect(lambda _checked=False: self._emit(self._pages))

        for button in (self.first_btn, self.prev_btn):
            row.addWidget(button)
        row.addStretch()
        row.addWidget(self.chips_host)
        row.addStretch()
        for button in (self.next_btn, self.last_btn):
            row.addWidget(button)
        row.addSpacing(SPACING_SMALL)
        row.addWidget(self.jump_label)
        row.addWidget(self.jump)

    def _step(self, delta: int) -> None:
        self._emit(self._page + delta)

    def _arrow(self, text: str, tooltip: str) -> QPushButton:
        button = QPushButton(text)
        button.setObjectName("pagerControl")
        button.setToolTip(tooltip)
        button.setAccessibleName(tooltip)
        return button

    def set_state(
        self,
        page: int,
        pages: int,
        entries: int,
        numbers: list[str] | None = None,
    ) -> None:
        """Point the pager at `page` of `pages`, a result set of `entries` rows.

        `numbers` carries the labels the rows actually show, in order. A release
        can number its episodes from anywhere (a later cour may start at 110), and
        a chip reading "1 - 140" above a grid starting at 110 is a lie. Without
        it the pager falls back to positional arithmetic, which is right for
        search results and only for releases that number from 1.
        """
        self._pages = max(1, pages)
        self._page = min(max(1, page), self._pages)
        self._entries = max(0, entries)
        self._numbers = list(numbers) if numbers else None
        self._render_chips()
        self.first_btn.setEnabled(self._page > 1)
        self.prev_btn.setEnabled(self._page > 1)
        self.next_btn.setEnabled(self._page < self._pages)
        self.last_btn.setEnabled(self._page < self._pages)
        self.jump_label.setText(f"сторінка з {self._pages}")
        self._syncing = True
        self.jump.setRange(1, self._pages)
        self.jump.setValue(self._page)
        self._syncing = False

    def _render_chips(self) -> None:
        while self._chip_layout.count():
            item = self._chip_layout.takeAt(0)
            if item.widget() is not None:
                item.widget().deleteLater()
        window = min(PAGER_CHIP_WINDOW, self._pages)
        first = min(max(0, self._page - 1 - window // 2), max(0, self._pages - window))
        for index in range(first, first + window):
            target = index + 1
            label = self._chip_label(target)
            button = QPushButton(label)
            button.setObjectName("pagerChipActive" if target == self._page else "pagerChip")
            button.setToolTip(f"Сторінка {target}")
            button.clicked.connect(lambda _checked=False, page=target: self._emit(page))
            self._chip_layout.addWidget(button)

    def _chip_label(self, target: int) -> str:
        """The real labels this page holds, so the chip matches the grid."""
        start = (target - 1) * self._page_size
        end = min(target * self._page_size, self._entries)
        if self._numbers and start < len(self._numbers):
            first = str(self._numbers[start])
            last = str(self._numbers[end - 1]) if end > start else first
            return f"{first} – {last}" if end > start else first
        return f"{start + 1} – {end}"

    def _on_jump(self, value: int) -> None:
        if not self._syncing:
            self._emit(value)

    def _emit(self, page: int) -> None:
        if 1 <= page <= self._pages and page != self._page:
            self.pageRequested.emit(page)
