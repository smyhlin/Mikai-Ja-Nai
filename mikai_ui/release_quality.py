"""A quality chip on each row of the release selector.

A QComboBox row is plain text, so the badge is painted by a delegate: it draws
the normal row first, then a rounded chip on the right carrying the highest
quality that release actually serves. The number comes from one lazy probe of
that release's first episode, never from a per-episode fetch.
"""

from __future__ import annotations

from PySide6.QtCore import QRect, QSize, Qt, Signal
from PySide6.QtGui import QColor, QFont, QPainter, QPainterPath
from PySide6.QtWidgets import QComboBox, QStyledItemDelegate

from mikai_ui.theme.colors import ACCENT, SURFACE_ACTIVE, TEXT
from mikai_ui.theme.metrics import RADIUS_SMALL, SPACING_MEDIUM

# Item data role carrying the highest height a release offers, 0 when unknown.
QUALITY_ROLE = int(Qt.ItemDataRole.UserRole) + 1

BADGE_HEIGHT = 20
BADGE_PADDING = 8
BADGE_GAP = SPACING_MEDIUM


class ReleaseSelector(QComboBox):
    """The release combo, reporting when its popup opens.

    A combo's popup is a private container, so `view()` exposes no aboutToShow
    signal; overriding showPopup is the one reliable hook, and the probes fire
    on first open rather than on every repaint.
    """

    popupShown = Signal()

    def showPopup(self) -> None:
        super().showPopup()
        self.popupShown.emit()


def max_quality(qualities: dict[int, str] | dict) -> int:
    """The highest usable height in a quality map, 0 when there is none."""
    heights = [
        int(height)
        for height, url in qualities.items()
        if isinstance(url, str) and url.strip() and height > 0
    ]
    return max(heights) if heights else 0


class ReleaseQualityDelegate(QStyledItemDelegate):
    """Paints the release row, then a quality chip on its right edge."""

    def sizeHint(self, option, index):  # noqa: N802 - Qt naming
        hint = super().sizeHint(option, index)
        return QSize(hint.width() + self._badge_width(option, index), hint.height())

    def paint(self, painter: QPainter, option, index) -> None:  # noqa: N802
        super().paint(painter, option, index)
        height = self._height_at(index)
        if height <= 0:
            return
        painter.save()
        self._paint_badge(painter, option, height)
        painter.restore()

    def _height_at(self, index) -> int:
        try:
            value = index.data(QUALITY_ROLE)
        except AttributeError, RuntimeError:
            return 0
        try:
            return int(value or 0)
        except TypeError, ValueError:
            return 0

    def _text(self, height: int) -> str:
        return f"{height}p"

    def _badge_text_width(self, painter: QPainter, height: int) -> int:
        return painter.fontMetrics().horizontalAdvance(self._text(height))

    def _badge_width(self, option, index) -> int:
        height = self._height_at(index)
        if height <= 0:
            return 0
        metrics = option.fontMetrics
        return metrics.horizontalAdvance(self._text(height)) + BADGE_PADDING * 2

    def _badge_rect(self, option, width: int, height: int) -> QRect:
        right = option.rect.right() - BADGE_GAP // 2
        top = option.rect.top() + (option.rect.height() - BADGE_HEIGHT) // 2
        return QRect(right - width, top, width, BADGE_HEIGHT)

    def _paint_badge(self, painter: QPainter, option, height: int) -> None:
        text = self._text(height)
        width = self._badge_text_width(painter, height) + BADGE_PADDING * 2
        rect = self._badge_rect(option, width, height)

        path = QPainterPath()
        path.addRoundedRect(rect, RADIUS_SMALL, RADIUS_SMALL)
        painter.fillPath(path, QColor(SURFACE_ACTIVE))
        painter.setPen(QColor(ACCENT))
        painter.drawPath(path)

        font = QFont(painter.font())
        font.setPointSizeF(max(8.0, font.pointSizeF() - 1.0))
        painter.setFont(font)
        painter.setPen(QColor(TEXT))
        metrics = painter.fontMetrics()
        text_rect = rect.adjusted(BADGE_PADDING, 0, -BADGE_PADDING, 0)
        painter.drawText(
            text_rect,
            int(Qt.AlignmentFlag.AlignCenter),
            metrics.elidedText(text, Qt.TextElideMode.ElideRight, text_rect.width()),
        )
