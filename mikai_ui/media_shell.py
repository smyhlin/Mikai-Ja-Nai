#!/usr/bin/env python3
"""
Enhanced Media Player Shell  ·  PySide6  ·  manifest v2.0.0
============================================================

A *shell only* implementation of the media-lightbox specification. It contains
no playback backend — you hand its video surface to libmpv and connect the
signals/slots listed under "BRIDGE" below.

    ┌──────────────────────────────────────────────────────────────┐
    │  PlayerShell (Qt)          ──signals──►   libmpv instance     │
    │                            ◄──slots───                       │
    └──────────────────────────────────────────────────────────────┘

 STATES (FSM)
    DEFAULT_FIT  ·  SCROLL_OUT  ·  ZOOM_IN_PAN
    FULLSCREEN_ACTIVE  ·  FULLSCREEN_AUTO_HIDDEN

 CHROME
    A  window controls   — □ ✕          top-right   z100
    B  flank chevrons    < >            left/right  z50
    C  source pill       Original File   bottom-center of frame  z30
    D  control dock      vol ▶ 0:00 ▬▬ -0:00 ⤢ 🗗 ⚙HD  bottom-center  z90
    E  settings flyout   speed slider + presets     z120
    F  footer            filename / origin · ⭳ ⋮    bottom-left / right  z40

 CANVAS
    wheel zoom 0.25×–5.0× (step 0.15, cursor-anchored) · drag pan
    (3 px threshold, clamp_to_edges) · double-click 2.2×

 QUICK START
    python shell.py                     # standalone, simulated clock
    python shell.py movie.mp4           # standalone with placeholder clock
    see run_mpv.py                      # real playback via python-mpv
"""

from __future__ import annotations

import math
import sys
import time
from pathlib import Path

from PySide6.QtCore import (
    QEasingCurve,
    QPoint,
    QPointF,
    QRect,
    QRectF,
    QSize,
    Qt,
    QTimer,
    QEvent,
    Signal,
    QVariantAnimation,
)
from PySide6.QtGui import (
    QBrush,
    QColor,
    QFont,
    QFontDatabase,
    QFontMetrics,
    QPainter,
    QPainterPath,
    QPen,
    QPolygonF,
    QRadialGradient,
)
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QGraphicsOpacityEffect,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

# ═══════════════════════════════════════════════════════════════════════════════
#  CONSTANTS
# ═══════════════════════════════════════════════════════════════════════════════

ZOOM_MIN, ZOOM_MAX = 0.25, 5.0
ZOOM_STEP = 0.15
ZOOM_DOUBLE_CLICK = 2.2
DRAG_THRESHOLD_PX = 3
AUTOHIDE_MS = 2500
MIN_WINDOW = QSize(720, 420)
FRAME_MARGIN = 8  # letterbox padding around the video surface
EDGE_GRIP = 8  # frameless resize hit band

ASPECT_FALLBACK = 16 / 9

# --- palette ---------------------------------------------------------------

C_BG_0 = QColor("#05060a")
C_DOCK = QColor(24, 25, 28, 230)
C_PILL = QColor(32, 34, 37, 217)
C_FLYOUT = QColor(35, 39, 45, 246)
C_BLACK_SOFT = QColor(0, 0, 0, 150)
C_ACCENT = QColor("#5865F2")
C_ACCENT_SOFT = QColor(88, 101, 242, 70)
C_GREEN = QColor("#3ba55d")
C_RED = QColor("#ed4245")


def _w(a: int) -> QColor:
    """white with alpha 0..255"""
    return QColor(255, 255, 255, a)


W_95, W_90, W_70, W_55, W_45, W_35, W_25, W_15, W_10 = (
    _w(242),
    _w(230),
    _w(179),
    _w(140),
    _w(115),
    _w(89),
    _w(64),
    _w(38),
    _w(26),
)

SPEED_PRESETS: list[tuple[float, str]] = [
    (0.5, "Повільна"),
    (1.0, "Звичайна"),
    (1.2, "Пришвидшена"),
    (1.5, "Швидка"),
    (1.7, "Дуже швидка"),
    (2.0, "Подвійна"),
]

SHORTCUTS: list[tuple[str, str]] = [
    ("Space", "toggle_playback"),
    ("F", "toggle_fullscreen_mode"),
    ("M", "toggle_mute"),
    ("Esc", "exit_viewer / reset_zoom"),
    ("←  →", "seek ∓5s / prev·next"),
    ("↑  ↓", "volume ±10 %"),
    ("+  −", "zoom_in / zoom_out"),
    ("R  0", "reset_canvas_transform"),
    ("D", "toggle_state_manifest_hud"),
    ("?", "this panel"),
]

STATE_DEFAULT_FIT = "DEFAULT_FIT"
STATE_SCROLL_OUT = "SCROLL_OUT"
STATE_ZOOM_IN_PAN = "ZOOM_IN_PAN"
STATE_FULLSCREEN_ACTIVE = "FULLSCREEN_ACTIVE"
STATE_FULLSCREEN_AUTO_HIDDEN = "FULLSCREEN_AUTO_HIDDEN"


# ═══════════════════════════════════════════════════════════════════════════════
#  FONTS & SMALL HELPERS
# ═══════════════════════════════════════════════════════════════════════════════

_SYS_MONO = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont).family()
_SYS_UI = QFontDatabase.systemFont(QFontDatabase.SystemFont.GeneralFont).family()


def mono(size: int = 10) -> QFont:
    f = QFont(_SYS_MONO)
    f.setPointSize(size)
    f.setStyleHint(QFont.StyleHint.Monospace)
    return f


def ui(size: int = 10) -> QFont:
    f = QFont(_SYS_UI)
    f.setPointSize(size)
    f.setFamilies([_SYS_UI, "Inter", "Segoe UI", "Noto Sans", "DejaVu Sans"])
    return f


def fmt_time(seconds: float) -> str:
    if seconds is None or not math.isfinite(seconds) or seconds < 0:
        seconds = 0.0
    m, s = divmod(int(seconds), 60)
    return f"{m:02d}:{s:02d}"


def fmt_signed(seconds: float) -> str:
    return "-" + fmt_time(seconds)


def rr(
    p: QPainter,
    rect: QRectF,
    radius: float,
    fill: QColor,
    border: QColor | None = None,
    border_w: float = 1.0,
) -> None:
    """paint a rounded rect"""
    p.setPen(Qt.PenStyle.NoPen if border is None else QPen(border, border_w))
    p.setBrush(QBrush(fill))
    p.drawRoundedRect(rect, radius, radius)


def text_width(font: QFont, s: str) -> int:
    return QFontMetrics(font).horizontalAdvance(s)


# ═══════════════════════════════════════════════════════════════════════════════
#  ICONS — painted procedurally on a 24×24 design grid
# ═══════════════════════════════════════════════════════════════════════════════


class Icon:
    """Each method draws one glyph into (x, y) at `size` px, 24-unit grid."""

    # ── plumbing ──────────────────────────────────────────────────────────
    @staticmethod
    def _stroke(p: QPainter, x: float, y: float, size: float, width: float = 1.8):
        p.save()
        p.translate(x, y)
        k = size / 24.0
        p.scale(k, k)
        pen = QPen(
            W_90,
            width / k,
            Qt.PenStyle.SolidLine,
            Qt.PenCapStyle.RoundCap,
            Qt.PenJoinStyle.RoundJoin,
        )
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(pen)

    @staticmethod
    def _fill(p: QPainter, x: float, y: float, size: float):
        p.save()
        p.translate(x, y)
        k = size / 24.0
        p.scale(k, k)
        p.setPen(Qt.PenStyle.NoPen)

    # ── window controls ───────────────────────────────────────────────────
    @staticmethod
    def minimize(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        p.setPen(QPen(c, 1.8 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.drawLine(QPointF(7, 12), QPointF(17, 12))
        p.restore()

    @staticmethod
    def restore(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        pen = QPen(c, 1.8 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        p.drawRoundedRect(QRectF(4, 8, 12, 12), 2, 2)
        p.drawLine(QPointF(8, 8), QPointF(8, 4))
        p.drawLine(QPointF(8, 4), QPointF(20, 4))
        p.drawLine(QPointF(20, 4), QPointF(20, 16))
        p.drawLine(QPointF(20, 16), QPointF(16, 16))
        p.restore()

    @staticmethod
    def close(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        pen = QPen(c, 1.8 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        p.drawLine(QPointF(7.5, 7.5), QPointF(16.5, 16.5))
        p.drawLine(QPointF(16.5, 7.5), QPointF(7.5, 16.5))
        p.restore()

    # ── navigation ────────────────────────────────────────────────────────
    @staticmethod
    def chevron_left(p: QPainter, x=0, y=0, s=18, c=W_90):
        Icon._stroke(p, x, y, s, 2.2)
        p.setPen(QPen(c, 2.2 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.drawLine(QPointF(14.5, 6), QPointF(8, 12))
        p.drawLine(QPointF(8, 12), QPointF(14.5, 18))
        p.restore()

    @staticmethod
    def chevron_right(p: QPainter, x=0, y=0, s=18, c=W_90):
        Icon._stroke(p, x, y, s, 2.2)
        p.setPen(QPen(c, 2.2 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.drawLine(QPointF(9.5, 6), QPointF(16, 12))
        p.drawLine(QPointF(16, 12), QPointF(9.5, 18))
        p.restore()

    # ── transport ─────────────────────────────────────────────────────────
    @staticmethod
    def play(p: QPainter, x=0, y=0, s=16, c=W_95):
        Icon._fill(p, x, y, s)
        p.setBrush(QBrush(c))
        p.drawPolygon(QPolygonF([QPointF(8, 5), QPointF(8, 19), QPointF(19.5, 12)]))
        p.restore()

    @staticmethod
    def pause(p: QPainter, x=0, y=0, s=16, c=W_95):
        Icon._fill(p, x, y, s)
        p.setBrush(QBrush(c))
        p.drawRoundedRect(QRectF(7, 5, 3.7, 14), 1.3, 1.3)
        p.drawRoundedRect(QRectF(13.3, 5, 3.7, 14), 1.3, 1.3)
        p.restore()

    # ── audio ─────────────────────────────────────────────────────────────
    @staticmethod
    def _speaker(p: QPainter, c=W_90):
        path = QPainterPath()
        path.moveTo(4.5, 9.5)
        path.lineTo(7.5, 9.5)
        path.lineTo(11.5, 5.8)
        path.lineTo(11.5, 18.2)
        path.lineTo(7.5, 14.5)
        path.lineTo(4.5, 14.5)
        path.closeSubpath()
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(c))
        p.drawPath(path)

    @staticmethod
    def volume(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        Icon._speaker(p, c)
        pen = QPen(c, 1.6 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(pen)
        p.drawArc(QRectF(13, 8, 4.5, 8), -52 * 16, 104 * 16)
        p.drawArc(QRectF(15.5, 5.5, 5.5, 13), -52 * 16, 104 * 16)
        p.restore()

    @staticmethod
    def volume_low(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        Icon._speaker(p, c)
        pen = QPen(c, 1.6 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(pen)
        p.drawArc(QRectF(13, 8, 4.5, 8), -52 * 16, 104 * 16)
        p.restore()

    @staticmethod
    def mute(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        Icon._speaker(p, c)
        pen = QPen(c, 1.8 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(pen)
        p.drawLine(QPointF(14.5, 9.5), QPointF(19.5, 14.5))
        p.drawLine(QPointF(19.5, 9.5), QPointF(14.5, 14.5))
        p.restore()

    # ── view mode ─────────────────────────────────────────────────────────
    @staticmethod
    def expand(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        pen = QPen(c, 1.8 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        p.drawLine(QPointF(14.5, 4.5), QPointF(19.5, 4.5))
        p.drawLine(QPointF(19.5, 4.5), QPointF(19.5, 9.5))
        p.drawLine(QPointF(19.5, 4.5), QPointF(13, 11))
        p.drawLine(QPointF(9.5, 19.5), QPointF(4.5, 19.5))
        p.drawLine(QPointF(4.5, 19.5), QPointF(4.5, 14.5))
        p.drawLine(QPointF(4.5, 19.5), QPointF(11, 13))
        p.restore()

    @staticmethod
    def contract(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        pen = QPen(c, 1.8 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        p.drawLine(QPointF(19.5, 9.5), QPointF(14.5, 9.5))
        p.drawLine(QPointF(14.5, 9.5), QPointF(19.5, 4.5))
        p.drawLine(QPointF(4.5, 14.5), QPointF(9.5, 14.5))
        p.drawLine(QPointF(9.5, 14.5), QPointF(4.5, 19.5))
        p.restore()

    @staticmethod
    def pip(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        p.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(W_70))
        p.drawRoundedRect(QRectF(12.5, 11.5, 6.5, 5.5), 1.2, 1.2)
        p.restore()

    # ── settings ──────────────────────────────────────────────────────────
    @staticmethod
    def gear(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        pen = QPen(c, 1.6 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        p.drawEllipse(QRectF(8.9, 8.9, 6.2, 6.2))
        for deg in range(0, 360, 45):
            rad = math.radians(deg)
            p.drawLine(
                QPointF(12 + 4.6 * math.cos(rad), 12 + 4.6 * math.sin(rad)),
                QPointF(12 + 6.6 * math.cos(rad), 12 + 6.6 * math.sin(rad)),
            )
        p.restore()

    # ── file / misc ───────────────────────────────────────────────────────
    @staticmethod
    def folder(p: QPainter, x=0, y=0, s=14, c=W_70):
        Icon._stroke(p, x, y, s)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(c))
        p.drawRoundedRect(QRectF(3.5, 5.5, 7.5, 3), 1, 1)
        p.drawRoundedRect(QRectF(3.5, 7.5, 17, 11.5), 1.6, 1.6)
        p.restore()

    @staticmethod
    def download(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._stroke(p, x, y, s)
        pen = QPen(c, 1.8 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        p.drawLine(QPointF(12, 4), QPointF(12, 12.5))
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(c))
        p.drawPolygon(QPolygonF([QPointF(7.8, 12), QPointF(16.2, 12), QPointF(12, 16.5)]))
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRoundedRect(QRectF(4, 18, 16, 2.5), 1.25, 1.25)
        p.restore()

    @staticmethod
    def more(p: QPainter, x=0, y=0, s=16, c=W_90):
        Icon._fill(p, x, y, s)
        p.setBrush(QBrush(c))
        for cy in (6, 12, 18):
            p.drawEllipse(QPointF(12, cy), 1.8, 1.8)
        p.restore()

    @staticmethod
    def reset(p: QPainter, x=0, y=0, s=14, c=W_90):
        Icon._stroke(p, x, y, s, 1.9)
        pen = QPen(c, 1.9 * 24 / s, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap)
        p.setPen(pen)
        p.drawArc(QRectF(5, 5, 14, 14), 35 * 16, 290 * 16)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(c))
        p.drawPolygon(QPolygonF([QPointF(17.6, 6.4), QPointF(17.6, 11.4), QPointF(13, 8.6)]))
        p.restore()

    @staticmethod
    def spark(p: QPainter, x=0, y=0, s=14, c=W_90):
        Icon._fill(p, x, y, s)
        p.setBrush(QBrush(c))
        p.drawPolygon(
            QPolygonF(
                [
                    QPointF(12, 3.5),
                    QPointF(13.6, 10.4),
                    QPointF(20.5, 12),
                    QPointF(13.6, 13.6),
                    QPointF(12, 20.5),
                    QPointF(10.4, 13.6),
                    QPointF(3.5, 12),
                    QPointF(10.4, 10.4),
                ]
            )
        )
        p.restore()

    @staticmethod
    def keyboard(p: QPainter, x=0, y=0, s=15, c=W_90):
        Icon._stroke(p, x, y, s)
        p.drawRoundedRect(QRectF(2.5, 6.5, 19, 11), 2, 2)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(c))
        for cx in (6.5, 9.7, 12.9, 16.1):
            p.drawEllipse(QPointF(cx, 10), 0.75, 0.75)
        p.drawRoundedRect(QRectF(8, 13.8, 8, 1.4), 0.7, 0.7)
        p.restore()


# ═══════════════════════════════════════════════════════════════════════════════
#  PRIMITIVE WIDGETS
# ═══════════════════════════════════════════════════════════════════════════════


class Panel(QWidget):
    """Opaque/translucent rounded container that paints its own background."""

    def __init__(
        self,
        fill: QColor = C_DOCK,
        radius: float = 10,
        border: QColor | None = W_15,
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        self._fill = fill
        self._radius = radius
        self._border = border
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)

    def set_fill(self, c: QColor):
        self._fill = c
        self.update()

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        rr(
            p,
            QRectF(0.5, 0.5, self.width() - 1, self.height() - 1),
            self._radius,
            self._fill,
            self._border,
        )


class IconButton(QPushButton):
    """Flat icon button; glyphs are painted, not pixmaps."""

    def __init__(
        self,
        icon_fn,
        tooltip: str = "",
        size: int = 30,
        glyph: int = 16,
        parent: QWidget | None = None,
        hover_fill: QColor | None = None,
    ):
        super().__init__(parent)
        self._icon = icon_fn
        self._glyph = glyph
        self._hover_fill = hover_fill if hover_fill is not None else _w(28)
        self._hover = False
        self._tint: QColor | None = None
        self.setFixedSize(size, size)
        self.setToolTip(tooltip)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFlat(True)

    def set_tint(self, c: QColor | None):
        self._tint = c
        self.update()

    def set_icon(self, fn):
        self._icon = fn
        self.update()

    def enterEvent(self, e):
        self._hover = True
        self.update()

    def leaveEvent(self, e):
        self._hover = False
        self.update()

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if self._hover or self.isChecked():
            rr(
                p,
                QRectF(1, 1, self.width() - 2, self.height() - 2),
                7,
                self._hover_fill,
                Qt.PenStyle.NoPen,
            )
        color = self._tint if self._tint is not None else W_90
        off = (self.width() - self._glyph) / 2
        self._icon(p, off, off, self._glyph, color)
        p.end()


class RangeSlider(QWidget):
    """Normalised (0..1) horizontal slider with hover preview."""

    valueChanged = Signal(float)
    pressed = Signal()
    released = Signal()

    def __init__(
        self, accent: QColor = W_95, track: int = 4, parent: QWidget | None = None, margin: int = 7
    ):
        super().__init__(parent)
        self._v = 0.0
        self._hover = -1.0
        self._accent = accent
        self._track = track
        self._margin = margin
        self._drag = False
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setMinimumHeight(22)

    # ── api ───────────────────────────────────────────────────────────────
    def value(self) -> float:
        return self._v

    def set_value(self, v: float, notify: bool = True):
        v = max(0.0, min(1.0, float(v)))
        if abs(v - self._v) > 1e-4:
            self._v = v
            if notify:
                self.valueChanged.emit(v)
            self.update()

    def set_value_silent(self, v: float):
        self.set_value(v, notify=False)

    def is_dragging(self) -> bool:
        return self._drag

    # ── geometry ──────────────────────────────────────────────────────────
    def _span(self) -> float:
        return max(1.0, self.width() - 2 * self._margin)

    def _at(self, x: float) -> float:
        return max(0.0, min(1.0, (x - self._margin) / self._span()))

    # ── events ────────────────────────────────────────────────────────────
    def mousePressEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton:
            self._drag = True
            self.pressed.emit()
            self.set_value(self._at(e.position().x()))

    def mouseMoveEvent(self, e):
        self._hover = self._at(e.position().x())
        if self._drag:
            self.set_value(self._hover)
        self.update()

    def mouseReleaseEvent(self, e):
        if self._drag:
            self._drag = False
            self.released.emit()

    def leaveEvent(self, e):
        self._hover = -1.0
        self.update()

    # ── paint ─────────────────────────────────────────────────────────────
    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        m = self._margin
        w = self._span()
        y = self.height() / 2 - self._track / 2
        half = self._track / 2

        rr(p, QRectF(m, y, w, self._track), half, W_25)
        if self._v > 0:
            rr(p, QRectF(m, y, w * self._v, self._track), half, self._accent)
        if 0 <= self._hover <= 1:
            rr(p, QRectF(m, y, w * min(self._hover, self._v), self._track), half, _w(60))

        tx = m + w * self._v
        cy = self.height() / 2
        r = 6.0 if (self.underMouse() or self._drag) else 5.0
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(QColor(0, 0, 0, 90)))
        p.drawEllipse(QPointF(tx, cy + 1), r, r)
        p.setBrush(QBrush(W_95))
        p.drawEllipse(QPointF(tx, cy), r - 1, r - 1)
        p.end()


class ScrubBar(RangeSlider):
    """Timeline with a mono timestamp tooltip on hover."""

    def __init__(self, parent=None):
        super().__init__(accent=W_95, parent=parent)
        self._duration = 0.0
        self._font = mono(9)

    def set_duration(self, d: float):
        self._duration = max(0.0, float(d or 0))

    def time_at(self, frac: float) -> float:
        return frac * self._duration

    def paintEvent(self, _):
        super().paintEvent(_)
        if self._duration <= 0 or not self.underMouse() or not (0 <= self._hover <= 1):
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        label = fmt_time(self._hover * self._duration)
        tw = text_width(self._font, label) + 12
        th = 17
        m = self._margin
        cx = m + self._span() * self._hover
        x = max(0, min(self.width() - tw, cx - tw / 2))
        rr(p, QRectF(x, 0, tw, th), 4, QColor(0, 0, 0, 205))
        p.setFont(self._font)
        p.setPen(W_90)
        p.drawText(QRectF(x, 0, tw, th), Qt.AlignmentFlag.AlignCenter, label)
        p.end()


class VolumeGroup(QWidget):
    """Speaker button + slider that expands on hover (45 px track)."""

    volumeChanged = Signal(float)
    muteClicked = Signal()

    TRACK = 45

    def __init__(self, parent=None):
        super().__init__(parent)
        self._vol = 0.7
        self._muted = False
        self._t = 0.0  # 0 = collapsed, 1 = expanded
        self._drag = False
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setMouseTracking(True)
        self.setFixedSize(28 + int(self.TRACK * 1.0), 30)
        self._anim = QVariantAnimation(self)
        self._anim.setDuration(180)
        self._anim.setEasingCurve(QEasingCurve.Type.OutCubic)
        self._anim.valueChanged.connect(self._on_anim)

    # ── api ───────────────────────────────────────────────────────────────
    def set_volume(self, v: float):
        self._vol = max(0.0, min(1.0, float(v)))
        self.update()

    def set_muted(self, m: bool):
        self._muted = m
        self.update()

    def effective(self) -> float:
        return 0.0 if self._muted else self._vol

    # ── animation ─────────────────────────────────────────────────────────
    def _on_anim(self, v):
        self._t = float(v)
        self.update()

    def _animate(self, target: float):
        self._anim.stop()
        self._anim.setStartValue(self._t)
        self._anim.setEndValue(target)
        self._anim.start()

    # ── events ────────────────────────────────────────────────────────────
    def enterEvent(self, e):
        self._animate(1.0)

    def leaveEvent(self, e):
        self._animate(0.0)
        self._drag = False

    def _over_speaker(self, x: float) -> bool:
        return x < 28

    def _over_track(self, x: float) -> bool:
        return 30 <= x <= 30 + self.TRACK

    def _frac(self, x: float) -> float:
        return max(0.0, min(1.0, (x - 30) / self.TRACK))

    def mousePressEvent(self, e):
        if e.button() != Qt.MouseButton.LeftButton:
            return
        x = e.position().x()
        if self._over_speaker(x):
            self.muteClicked.emit()
        elif self._over_track(x):
            self._drag = True
            self.volumeChanged.emit(self._frac(x))

    def mouseMoveEvent(self, e):
        x = e.position().x()
        if self._drag and e.buttons() & Qt.MouseButton.LeftButton:
            self.volumeChanged.emit(self._frac(x))

    def mouseReleaseEvent(self, e):
        self._drag = False

    # ── paint ─────────────────────────────────────────────────────────────
    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        eff = self.effective()

        if eff == 0:
            Icon.mute(p, 6, 7, 16)
        elif eff < 0.5:
            Icon.volume_low(p, 6, 7, 16)
        else:
            Icon.volume(p, 6, 7, 16)

        if self._t > 0.01:
            p.setOpacity(self._t)
            x0, w = 30, self.TRACK
            rr(p, QRectF(x0, 13, w, 4), 2, W_25)
            if eff > 0:
                rr(p, QRectF(x0, 13, w * eff, 4), 2, W_95)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QBrush(W_95))
            p.drawEllipse(QPointF(x0 + w * eff, 15), 4, 4)
            p.setOpacity(1.0)
        p.end()


# ═══════════════════════════════════════════════════════════════════════════════
#  E · SETTINGS FLYOUT
# ═══════════════════════════════════════════════════════════════════════════════


class SettingsFlyout(QFrame):
    """190 px popover: continuous speed slider + preset list + quality row."""

    speedSelected = Signal(float)

    def __init__(self, parent=None):
        super().__init__(parent, Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setStyleSheet(
            "SettingsFlyout {"
            "  background-color: rgba(35,39,45,246);"
            "  border: 1px solid rgba(255,255,255,38);"
            "  border-radius: 9px;"
            "}"
        )
        self._rate = 1.0
        self._quality = "HD"

        lay = QVBoxLayout(self)
        lay.setContentsMargins(10, 10, 10, 10)
        lay.setSpacing(5)

        head = QHBoxLayout()
        cap = QLabel("ШВИДКІСТЬ")
        cap.setFont(mono(8))
        cap.setStyleSheet("color: rgba(255,255,255,115); letter-spacing: 2px;")
        head.addWidget(cap)
        head.addStretch()
        self._badge = QLabel("1.0x")
        self._badge.setFont(mono(9))
        self._badge.setStyleSheet(
            "background:#5865F2; color:#fff; border-radius:4px;padding:1px 6px; font-weight:700;"
        )
        head.addWidget(self._badge)
        lay.addLayout(head)

        self._slider = RangeSlider(accent=C_ACCENT, parent=self)
        self._slider.valueChanged.connect(self._on_slider)
        lay.addWidget(self._slider)

        sep = QFrame(self)
        sep.setFixedHeight(1)
        sep.setStyleSheet("background: rgba(255,255,255,25);")
        lay.addWidget(sep)

        self._rows: list[tuple[float, QPushButton]] = []
        for speed, label in SPEED_PRESETS:
            btn = QPushButton(f"{speed:g}x   {label}", self)
            btn.setFont(ui(10))
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.setFixedHeight(26)
            btn.clicked.connect(lambda _=False, s=speed: self._pick(s))
            lay.addWidget(btn)
            self._rows.append((speed, btn))

        sep2 = QFrame(self)
        sep2.setFixedHeight(1)
        sep2.setStyleSheet("background: rgba(255,255,255,25);")
        lay.addWidget(sep2)

        qrow = QHBoxLayout()
        ql = QLabel("ЯКІСТЬ")
        ql.setFont(mono(8))
        ql.setStyleSheet("color: rgba(255,255,255,100); letter-spacing: 1.5px;")
        qrow.addWidget(ql)
        qrow.addStretch()
        self._qlabel = QLabel("HD")
        self._qlabel.setFont(mono(9))
        self._qlabel.setStyleSheet("color:#3ba55d; font-weight:700;")
        qrow.addWidget(self._qlabel)
        lay.addLayout(qrow)

        self._refresh()

    # ── api ───────────────────────────────────────────────────────────────
    def set_rate(self, r: float):
        self._rate = max(0.25, min(2.0, float(r)))
        self._refresh()

    def set_quality(self, q: str):
        self._quality = q
        self._refresh()

    def popup_near(self, anchor_global: QPoint):
        self._refresh()
        self.setFixedSize(self.sizeHint())
        x = anchor_global.x() + 14 - self.width()
        y = anchor_global.y() - self.height() - 10
        self.move(max(4, x), max(4, y))
        self.show()
        self.raise_()

    # ── internals ─────────────────────────────────────────────────────────
    def _pick(self, speed: float):
        self._rate = speed
        self._refresh()
        self.speedSelected.emit(speed)
        self.close()

    def _on_slider(self, frac: float):
        self._rate = 0.25 + frac * 1.75
        self._refresh()
        self.speedSelected.emit(round(self._rate, 2))

    def _refresh(self):
        self._badge.setText(f"{self._rate:g}x")
        self._qlabel.setText(self._quality)
        self._slider.set_value_silent((self._rate - 0.25) / 1.75)
        for speed, btn in self._rows:
            sel = abs(self._rate - speed) < 0.026
            btn.setText(("✓  " if sel else "") + f"{speed:g}x   " + dict(SPEED_PRESETS)[speed])
            fg = "#ffffff" if sel else "rgba(255,255,255,175)"
            bg = "rgba(255,255,255,30)" if sel else "transparent"
            btn.setStyleSheet(
                "QPushButton {"
                f"  color: {fg};"
                f"  background: {bg};"
                "  border: none; border-radius: 5px; text-align: left;"
                "  padding: 0 8px;"
                "}"
                "QPushButton:hover { background: rgba(255,255,255,22); }"
            )


# ═══════════════════════════════════════════════════════════════════════════════
#  OVERLAYS · STATE BADGE / HUD / TOAST / KEY HINTS
# ═══════════════════════════════════════════════════════════════════════════════


class StateBadge(QWidget):
    """Small pill showing the live ViewerStateId."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._state = STATE_DEFAULT_FIT
        self._font = mono(8)
        self.setMinimumHeight(22)
        self._resize()

    def set_state(self, s: str):
        self._state = s
        self._resize()
        self.update()

    def _resize(self):
        self.setFixedSize(text_width(self._font, self._state) + 34, 22)

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        fs = self._state.startswith("FULLSCREEN_ACTIVE")
        rr(
            p,
            QRectF(0.5, 0.5, self.width() - 1, self.height() - 1),
            11,
            C_ACCENT_SOFT if fs else C_BLACK_SOFT,
            W_15,
        )
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(C_GREEN))
        p.drawEllipse(QPointF(11, self.height() / 2), 3, 3)
        p.setFont(self._font)
        p.setPen(W_70)
        p.drawText(
            QRectF(18, 0, self.width() - 18, self.height()),
            Qt.AlignmentFlag.AlignVCenter,
            self._state,
        )


class StateHUD(Panel):
    """Live manifest readout (scale, offset, cursor, position…)."""

    def __init__(self, parent=None):
        super().__init__(QColor(0, 0, 0, 150), 9, W_15, parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self._rows: list[tuple[QLabel, QLabel]] = []

        lay = QVBoxLayout(self)
        lay.setContentsMargins(11, 9, 11, 9)
        lay.setSpacing(1)

        head = QHBoxLayout()
        h1 = QLabel("state")
        h1.setFont(mono(8))
        h1.setStyleSheet("color:#7289ff; letter-spacing:2px;")
        head.addWidget(h1)
        head.addStretch()
        h2 = QLabel("live")
        h2.setFont(mono(8))
        h2.setStyleSheet("color:#3ba55d; letter-spacing:2px;")
        head.addWidget(h2)
        lay.addLayout(head)

        self._title = QLabel(STATE_DEFAULT_FIT)
        self._title.setFont(mono(9))
        self._title.setStyleSheet("color:#7289ff;")
        lay.addWidget(self._title)
        lay.addSpacing(4)

        for _ in range(7):
            k = QLabel("")
            k.setFont(mono(8))
            k.setStyleSheet("color: rgba(255,255,255,95);")
            v = QLabel("")
            v.setFont(mono(8))
            v.setStyleSheet("color: rgba(255,255,255,220);")
            row = QHBoxLayout()
            row.setSpacing(6)
            row.addWidget(k, 0)
            row.addStretch()
            row.addWidget(v, 0)
            lay.addLayout(row)
            self._rows.append((k, v))

        foot = QLabel("D — hide · manifest v2.0.0")
        foot.setFont(mono(7))
        foot.setStyleSheet("color: rgba(255,255,255,70);")
        lay.addSpacing(3)
        lay.addWidget(foot)

        self.setFixedWidth(212)

    def update_data(self, state: str, data: list[tuple[str, str]]):
        self._title.setText(state)
        for i, (k, v) in enumerate(self._rows):
            if i < len(data):
                k.setText(data[i][0])
                v.setText(data[i][1])
            else:
                k.setText("")
                v.setText("")
        self.adjustSize()


class CenterGlyph(QWidget):
    """Big translucent play/pause disc shown when paused."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._kind = "play"  # "play" | "pause" | None
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setFixedSize(74, 74)
        self.hide()

    def set_kind(self, kind):
        self._kind = kind
        if kind is None:
            self.hide()
        else:
            self.show()
            self.update()

    def paintEvent(self, _):
        if self._kind is None:
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        rr(p, QRectF(0.5, 0.5, 73, 73), 37, QColor(0, 0, 0, 130), W_25)
        off = 74 / 2 - 22 / 2
        if self._kind == "play":
            Icon.play(p, off + 3, off, 22, W_95)
        else:
            Icon.pause(p, off, off, 22, W_95)
        p.end()


class PillButton(QWidget):
    """'Original File' — folder glyph + label, click-emitting."""

    clicked = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._text = "Original File"
        self._font = ui(10)
        self._hover = False
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFixedSize(text_width(self._font, self._text) + 26, 22)

    def enterEvent(self, e):
        self._hover = True
        self.update()

    def leaveEvent(self, e):
        self._hover = False
        self.update()

    def mousePressEvent(self, e):
        if e.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit()

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        c = W_95 if self._hover else W_70
        Icon.folder(p, 1, 4, 14, c)
        p.setFont(self._font)
        p.setPen(c)
        p.drawText(
            QRectF(19, 0, self.width() - 19, self.height()),
            Qt.AlignmentFlag.AlignVCenter,
            self._text,
        )
        p.end()


class Toast(Panel):
    """Transient centred notification."""

    def __init__(self, parent=None):
        super().__init__(QColor(0, 0, 0, 205), 8, W_15, parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)

        self._effect = QGraphicsOpacityEffect(self)
        self._effect.setOpacity(0.0)
        self.setGraphicsEffect(self._effect)
        self._label = QLabel(self)
        self._label.setFont(ui(10))
        self._label.setStyleSheet("color: rgba(255,255,255,235);")
        self._label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(14, 8, 14, 8)
        lay.addWidget(self._label)
        self.hide()

        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self._begin_out)

        self._fade = QVariantAnimation(self)
        self._fade.setDuration(190)
        self._fade.valueChanged.connect(self._set_opacity)
        self._fade.finished.connect(self._on_done)

        self._op = 0.0
        self._set_opacity(0.0)

    def _set_opacity(self, v: float):
        self._op = float(v)
        self._effect.setOpacity(self._op)
        self.setVisible(self._op > 0.01)

    def _on_done(self):
        if self._op <= 0.01:
            self.hide()

    def _begin_out(self):
        self._fade.stop()
        self._fade.setStartValue(self._op)
        self._fade.setEndValue(0.0)
        self._fade.start()

    def flash(self, text: str, ms: int = 1900):
        self._label.setText(text)
        self.adjustSize()
        self._fade.stop()
        self._fade.setStartValue(self._op)
        self._fade.setEndValue(1.0)
        self._fade.start()
        self._timer.start(ms)


class KeyHints(QFrame):
    """Popup listing supported_input_modalities."""

    def __init__(self, parent=None):
        super().__init__(parent, Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setStyleSheet(
            "KeyHints {"
            "  background-color: rgba(35,39,45,246);"
            "  border: 1px solid rgba(255,255,255,38);"
            "  border-radius: 9px;"
            "}"
        )
        lay = QVBoxLayout(self)
        lay.setContentsMargins(11, 10, 11, 10)
        lay.setSpacing(3)
        cap = QLabel("supported_input_modalities")
        cap.setFont(mono(8))
        cap.setStyleSheet("color: rgba(255,255,255,115); letter-spacing:1.5px;")
        lay.addWidget(cap)
        lay.addSpacing(2)
        for key, action in SHORTCUTS:
            row = QHBoxLayout()
            row.setSpacing(8)
            k = QLabel(key)
            k.setFont(mono(8))
            k.setAlignment(Qt.AlignmentFlag.AlignCenter)
            k.setFixedWidth(58)
            k.setStyleSheet(
                "color: rgba(255,255,255,225); background: rgba(0,0,0,90);"
                "border: 1px solid rgba(255,255,255,30); border-radius: 4px;"
                "padding: 2px 0;"
            )
            a = QLabel(action)
            a.setFont(mono(8))
            a.setStyleSheet("color: rgba(255,255,255,140);")
            row.addWidget(k)
            row.addWidget(a)
            lay.addLayout(row)

    def popup_near(self, anchor_global: QPoint):
        self.setFixedSize(self.sizeHint())
        self.move(max(4, anchor_global.x()), max(4, anchor_global.y() + 18))
        self.show()
        self.raise_()


# ═══════════════════════════════════════════════════════════════════════════════
#  MAIN SHELL
# ═══════════════════════════════════════════════════════════════════════════════


class PlayerShell(QWidget):
    """
    The lightbox shell.  No playback logic — see the BRIDGE section at the
    bottom of the class for the exact signal/slot contract.
    """

    # ── BRIDGE: shell → host (apply these to mpv) ─────────────────────────
    playPauseRequested = Signal()  # toggle_playback
    seekRequested = Signal(float)  # absolute seconds
    rateChanged = Signal(float)  # 0.25 … 2.0
    volumeChanged = Signal(float)  # 0 … 1
    muteChanged = Signal(bool)
    fullscreenChanged = Signal(bool)
    pipRequested = Signal()
    nextRequested = Signal()  # load_next_media
    prevRequested = Signal()  # load_previous_media
    downloadRequested = Signal()
    openOriginalRequested = Signal()
    mediaDropped = Signal(str)  # local file path
    closeRequested = Signal()  # exit_viewer
    stateChanged = Signal(str)  # ViewerStateId

    def __init__(self, parent=None, frameless: bool = True, demo_clock: bool = True):
        super().__init__(parent)
        self.setWindowTitle("Media Lightbox · manifest 2.0.0")
        self.setMinimumSize(MIN_WINDOW)
        self.resize(1280, 760)
        self.setAcceptDrops(True)
        self.setMouseTracking(True)

        if frameless:
            self.setWindowFlags(Qt.WindowType.Window | Qt.WindowType.FramelessWindowHint)
            self._frameless = True
        else:
            self._frameless = False

        # ── canvas / fsm ──────────────────────────────────────────────
        self._scale = 1.0
        self._offset = QPointF(0, 0)
        self._panning = False
        self._fullscreen = False
        self._hud_hidden = False
        self._rounded = True  # toggle_window_mode
        self._aspect = ASPECT_FALLBACK
        # off in mikai: the dock already shows time, quality and rate, and the
        # state panel repeated them in the corner of every playback
        self._show_hud = False

        # ── media ─────────────────────────────────────────────────────
        self._path = ""
        self._duration = 0.0
        self._position = 0.0
        self._playing = False
        self._volume = 0.7
        self._muted = False
        self._rate = 1.0
        self._quality = "HD"
        self._requester = "Dmitry"
        self._origin = "UDream"
        self._has_media = False

        # ── demo clock (standalone use only) ──────────────────────────
        self._demo = demo_clock
        self._demo_timer = QTimer(self)
        self._demo_timer.setInterval(200)
        self._demo_timer.timeout.connect(self._demo_tick)

        # ── build ─────────────────────────────────────────────────────
        self._build_surface()
        self._build_window_controls()
        self._build_badge()
        self._build_chevrons()
        self._build_pill()
        self._build_dock()
        self._build_footer()
        self._build_overlays()

        self._hide_timer = QTimer(self)
        self._hide_timer.setSingleShot(True)
        self._hide_timer.timeout.connect(self._enter_auto_hidden)

        self._refresh_timer = QTimer(self)
        self._refresh_timer.setInterval(200)
        self._refresh_timer.timeout.connect(self._refresh_hud)

        self._press = QPointF()
        self._press_offset = QPointF()
        self._moved = False
        self._dragging_window = False
        self._drag_geom = QRect()
        self._drag_origin = QPoint()
        self._resizing: tuple[bool, bool, bool, bool] = (False, False, False, False)
        self._grabbed = False

        self._apply_state()
        self._layout()

    # ═════════════════════════════════════════════════════════════════
    #  BUILDERS
    # ═════════════════════════════════════════════════════════════════

    def _build_surface(self) -> None:
        """z10 · media-canvas-viewport.  Hand this widget's winId() to mpv."""
        self._surface = QWidget(self)
        self._surface.setObjectName("videoSurface")
        self._surface.setStyleSheet(
            "QWidget#videoSurface { background: #000; border-radius: 12px; }"
        )
        self._surface.setMouseTracking(True)
        self._surface.installEventFilter(self)

    def _build_window_controls(self) -> None:
        """z100 · top-right"""
        self._wc = Panel(C_BLACK_SOFT, 8, W_15, self)
        self._wc.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, False)
        lay = QHBoxLayout(self._wc)
        lay.setContentsMargins(4, 4, 4, 4)
        lay.setSpacing(2)

        self._btn_min = IconButton(Icon.minimize, "window_minimize", 28, 15, self._wc)
        self._btn_min.clicked.connect(self.showMinimized)
        lay.addWidget(self._btn_min)

        self._btn_win = IconButton(Icon.restore, "toggle_window_mode", 28, 15, self._wc)
        self._btn_win.clicked.connect(self._toggle_window_mode)
        lay.addWidget(self._btn_win)

        self._btn_close = IconButton(
            Icon.close, "exit_viewer  (Esc)", 28, 15, self._wc, hover_fill=QColor(237, 66, 69, 190)
        )
        self._btn_close.clicked.connect(self._exit)
        lay.addWidget(self._btn_close)

    def _build_badge(self) -> None:
        self._badge = StateBadge(self)

    def _build_chevrons(self) -> None:
        """z50 · flank navigation"""
        self._btn_prev = IconButton(
            Icon.chevron_left,
            "load_previous_media  (←)",
            42,
            18,
            self,
            hover_fill=QColor(0, 0, 0, 165),
        )
        self._btn_prev.clicked.connect(self.prevRequested.emit)
        self._btn_prev.set_tint(W_70)

        self._btn_next = IconButton(
            Icon.chevron_right,
            "load_next_media  (→)",
            42,
            18,
            self,
            hover_fill=QColor(0, 0, 0, 165),
        )
        self._btn_next.clicked.connect(self.nextRequested.emit)
        self._btn_next.set_tint(W_70)

    def _build_pill(self) -> None:
        """z30 · bottom-center of the media frame"""
        self._pill = Panel(C_PILL, 15, QColor(255, 255, 255, 22), self)
        lay = QHBoxLayout(self._pill)
        lay.setContentsMargins(12, 4, 12, 4)
        lay.setSpacing(8)

        self._btn_original = PillButton(self._pill)
        self._btn_original.setToolTip("open original file")
        self._btn_original.clicked.connect(self.openOriginalRequested.emit)
        lay.addWidget(self._btn_original)

        dot = QLabel("·", self._pill)
        dot.setStyleSheet("color: rgba(255,255,255,45);")
        lay.addWidget(dot)

        self._lbl_requester = QLabel(f"Requested by: {self._requester}", self._pill)
        self._lbl_requester.setFont(ui(9))
        self._lbl_requester.setStyleSheet("color: rgba(255,255,255,115);")
        lay.addWidget(self._lbl_requester)
        self._pill.adjustSize()

    def _build_dock(self) -> None:
        """z90 · bottom-center"""
        self._dock = Panel(C_DOCK, 9, W_15, self)
        lay = QHBoxLayout(self._dock)
        lay.setContentsMargins(11, 7, 11, 7)
        lay.setSpacing(8)

        self._vol = VolumeGroup(self._dock)
        self._vol.volumeChanged.connect(self._on_volume)
        self._vol.muteClicked.connect(self._toggle_mute)
        lay.addWidget(self._vol)

        self._btn_play = IconButton(Icon.play, "toggle_playback  (Space)", 28, 15, self._dock)
        self._btn_play.clicked.connect(self._on_play_clicked)
        lay.addWidget(self._btn_play)

        self._lbl_elapsed = QLabel("00:00", self._dock)
        self._lbl_elapsed.setFont(mono(9))
        self._lbl_elapsed.setStyleSheet("color: rgba(255,255,255,230);")
        self._lbl_elapsed.setFixedWidth(34)
        lay.addWidget(self._lbl_elapsed)

        self._scrub = ScrubBar(self._dock)
        self._scrub.setMinimumWidth(120)
        self._scrub.valueChanged.connect(self._on_scrub)
        lay.addWidget(self._scrub, 1)

        self._lbl_remaining = QLabel("-00:00", self._dock)
        self._lbl_remaining.setFont(mono(9))
        self._lbl_remaining.setStyleSheet("color: rgba(255,255,255,140);")
        self._lbl_remaining.setFixedWidth(38)
        lay.addWidget(self._lbl_remaining)

        lay.addSpacing(2)

        self._btn_fs = IconButton(Icon.expand, "toggle_fullscreen_mode  (F)", 28, 15, self._dock)
        self._btn_fs.clicked.connect(self._toggle_fullscreen)
        lay.addWidget(self._btn_fs)

        self._btn_pip = IconButton(Icon.pip, "toggle_picture_in_picture", 28, 15, self._dock)
        self._btn_pip.clicked.connect(self.pipRequested.emit)
        lay.addWidget(self._btn_pip)

        self._btn_settings = IconButton(Icon.gear, "toggle_settings_popover", 28, 15, self._dock)
        self._btn_settings.clicked.connect(self._open_settings)
        lay.addWidget(self._btn_settings)

        self._flyout = SettingsFlyout(self)
        self._flyout.speedSelected.connect(self._on_rate)

        # HD badge drawn over the gear
        self._dock.adjustSize()

    def _build_footer(self) -> None:
        """z40 · bottom-left info + bottom-right actions"""
        self._footer_l = QWidget(self)
        self._footer_l.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        v = QVBoxLayout(self._footer_l)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(1)
        self._lbl_file = QLabel("no media loaded", self._footer_l)
        self._lbl_file.setFont(mono(8))
        self._lbl_file.setStyleSheet("color: rgba(255,255,255,130);")
        v.addWidget(self._lbl_file)
        self._lbl_origin = QLabel("drop a file · or pass one as argv[1]", self._footer_l)
        self._lbl_origin.setFont(ui(9))
        self._lbl_origin.setStyleSheet("color: rgba(255,255,255,85);")
        v.addWidget(self._lbl_origin)

        self._footer_r = QWidget(self)
        self._footer_r.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        h = QHBoxLayout(self._footer_r)
        h.setContentsMargins(0, 0, 0, 0)
        h.setSpacing(4)

        self._btn_reset = IconButton(
            Icon.reset, "reset_canvas_transform  (R / 0)", 28, 14, self._footer_r
        )
        self._btn_reset.clicked.connect(self.reset_transform)
        h.addWidget(self._btn_reset)

        self._btn_keys = IconButton(
            Icon.keyboard, "keyboard_shortcuts  (?)", 28, 15, self._footer_r
        )
        self._btn_keys.clicked.connect(self._open_keys)
        h.addWidget(self._btn_keys)

        self._btn_hud = IconButton(
            Icon.spark, "toggle_state_manifest_hud  (D)", 28, 14, self._footer_r
        )
        self._btn_hud.clicked.connect(self.toggle_hud)
        self._btn_hud.set_tint(C_ACCENT)
        self._btn_hud.hide()  # same reason the panel itself is off
        h.addWidget(self._btn_hud)

        self._btn_dl = IconButton(Icon.download, "download_media_file", 28, 16, self._footer_r)
        self._btn_dl.clicked.connect(self.downloadRequested.emit)
        h.addWidget(self._btn_dl)

        self._btn_more = IconButton(Icon.more, "open_extended_menu", 28, 16, self._footer_r)
        self._btn_more.clicked.connect(self._open_menu)
        h.addWidget(self._btn_more)

        self._keys = KeyHints(self)

    def _build_overlays(self) -> None:
        self._hud = StateHUD(self)
        self._toast = Toast(self)

        # big centre play glyph for the paused state
        self._center = CenterGlyph(self)
        self._center.set_kind(None)

    # ═════════════════════════════════════════════════════════════════
    #  STATE MACHINE
    # ═════════════════════════════════════════════════════════════════

    def state(self) -> str:
        if self._hud_hidden:
            return STATE_FULLSCREEN_AUTO_HIDDEN
        if self._fullscreen:
            return STATE_FULLSCREEN_ACTIVE
        if self._scale > 1.005:
            return STATE_ZOOM_IN_PAN
        if self._scale < 0.995:
            return STATE_SCROLL_OUT
        return STATE_DEFAULT_FIT

    def _apply_state(self) -> None:
        prev = getattr(self, "_last_state", None)
        st = self.state()
        self._last_state = st

        fs, hidden, zoom = self._fullscreen, self._hud_hidden, self._scale > 1.005

        # chrome visibility matrix
        self._wc.setVisible(not hidden)
        self._badge.setVisible(not hidden)
        self._btn_prev.setVisible(not hidden and not fs and not zoom)
        self._btn_next.setVisible(not hidden and not fs and not zoom)
        self._pill.setVisible(not hidden and not fs)
        self._dock.setVisible(not hidden)
        self._footer_l.setVisible(not hidden and not fs)
        self._footer_r.setVisible(not hidden and not fs)
        self._hud.setVisible(not hidden and self._show_hud)
        if hidden:
            self._flyout.close()
            self._keys.close()

        self._set_state_cursor()

        # window-mode chrome (toggle_window_mode → rounded vs. hard-edged frame)
        radius = 12 if (self._rounded and not fs) else 0
        ring = "border: 1px solid rgba(255,255,255,28);" if radius else ""
        self._surface.setStyleSheet(
            f"QWidget#videoSurface {{ background:#000; border-radius:{radius}px; {ring} }}"
        )

        if st != prev:
            self.stateChanged.emit(st)
        self._layout()
        self._refresh_hud()

    def _set_state_cursor(self) -> None:
        """Cursor per the manifest's 'Mouse Cursor' row."""
        if self._hud_hidden:
            self.setCursor(Qt.CursorShape.BlankCursor)
        elif self._scale > 1.005:
            self.setCursor(
                Qt.CursorShape.ClosedHandCursor if self._panning else Qt.CursorShape.OpenHandCursor
            )
        elif self._scale < 0.995:
            self.setCursor(Qt.CursorShape.PointingHandCursor)
        else:
            self.setCursor(Qt.CursorShape.ArrowCursor)

    # ── fullscreen / autohide ────────────────────────────────────────

    def _toggle_fullscreen(self) -> None:
        if self._fullscreen:
            self._exit_fullscreen()
        else:
            self._enter_fullscreen()

    def _enter_fullscreen(self) -> None:
        self._fullscreen = True
        self._hud_hidden = False
        self.showFullScreen()
        self._btn_fs.set_icon(Icon.contract)
        self._apply_state()
        self._hide_timer.start(AUTOHIDE_MS)
        self.fullscreenChanged.emit(True)

    def _exit_fullscreen(self) -> None:
        self._fullscreen = False
        self._hud_hidden = False
        self._hide_timer.stop()
        self.showNormal()
        self.resize(1280, 760)
        self._btn_fs.set_icon(Icon.expand)
        self._apply_state()
        self.fullscreenChanged.emit(False)

    def _enter_auto_hidden(self) -> None:
        if self._fullscreen and not self.underMouse():
            self._hud_hidden = True
            self._apply_state()

    def _wake(self) -> None:
        if self._hud_hidden:
            self._hud_hidden = False
            self._apply_state()
        if self._fullscreen:
            self._hide_timer.start(AUTOHIDE_MS)

    # ═════════════════════════════════════════════════════════════════
    #  LAYOUT
    # ═════════════════════════════════════════════════════════════════

    def _fitted(self) -> tuple[int, int]:
        pad = 0 if self._fullscreen else FRAME_MARGIN
        chrome_h = 0 if self._fullscreen else 150
        vw = max(160, self.width() - 2 * (pad + 24))
        vh = max(100, self.height() - chrome_h)
        if vw / vh > self._aspect:
            return int(vh * self._aspect), int(vh)
        return int(vw), int(vw / self._aspect)

    def _layout(self) -> None:
        w, h = self.width(), self.height()

        # ── z10 canvas ──────────────────────────────────────────────
        sw, sh = self._fitted()
        sx = (w - sw) / 2 + self._offset.x()
        sy = (h - sh) / 2 + self._offset.y()
        self._surface.setGeometry(int(sx), int(sy), sw, sh)

        # ── z100 window controls (top-right, 94.5–99.5 % / 0.8–3.5 %) ─
        wc_w = self._wc.sizeHint().width() or 96
        self._wc.setGeometry(int(w * 0.945) + 4, max(6, int(h * 0.008)), wc_w, 36)
        self._badge.move(self._wc.x() - self._badge.width() - 8, self._wc.y() + 7)

        # ── z50 chevrons ────────────────────────────────────────────
        cy = int(h * 0.5) - 21
        self._btn_prev.move(int(w * 0.012), cy)
        self._btn_next.move(w - int(w * 0.012) - 42, cy)

        # ── z30 source pill (86.5–91.5 % of the media frame) ────────
        self._pill.adjustSize()
        pill_bottom = sy + sh * 0.90
        self._pill.move(
            int(sx + (sw - self._pill.width()) / 2), int(pill_bottom - self._pill.height())
        )

        # ── z90 dock (93–98 % of height, centred) ───────────────────
        dw = min(self._dock.sizeHint().width() + 22, w - 24)
        self._dock.setGeometry((w - dw) // 2, h - int(h * 0.022) - 46, dw, 46)

        # ── z40 footer ──────────────────────────────────────────────
        self._footer_l.adjustSize()
        self._footer_l.move(int(w * 0.010), h - self._footer_l.height() - 8)
        self._footer_r.adjustSize()
        self._footer_r.move(
            w - self._footer_r.width() - int(w * 0.010), h - self._footer_r.height() - 8
        )

        # ── overlays ────────────────────────────────────────────────
        self._hud.adjustSize()
        self._hud.move(w - self._hud.width() - 14, int(h * 0.30))
        self._center.move((w - 74) // 2, (h - 74) // 2)
        if self._toast.isVisible():
            self._toast.move((w - self._toast.width()) // 2, int(h * 0.80))

    def resizeEvent(self, e):
        super().resizeEvent(e)
        self._layout()

    def _refresh_hud(self) -> None:
        self._hud.update_data(
            self.state(),
            [
                ("scale", f"{self._scale:.2f}"),
                (
                    "pan_origin",
                    f"{50 + self._offset.x() / max(1, self.width()) * 100:.1f}%, "
                    f"{50 + self._offset.y() / max(1, self.height()) * 100:.1f}%",
                ),
                ("offset", f"{int(self._offset.x())}, {int(self._offset.y())}"),
                ("cursor", self._cursor_name()),
                ("t", f"{fmt_time(self._position)} / {fmt_time(self._duration)}"),
                ("rate", f"{self._rate:g}x"),
                ("volume", f"{int(self._volume * 100)}%" + ("  muted" if self._muted else "")),
            ],
        )
        self._hud.adjustSize()
        if self._show_hud and not self._hud_hidden:
            self._hud.move(self.width() - self._hud.width() - 14, int(self.height() * 0.30))

    def _cursor_name(self) -> str:
        st = self.state()
        if st == STATE_FULLSCREEN_AUTO_HIDDEN:
            return "none"
        if st == STATE_ZOOM_IN_PAN:
            return "grabbing" if self._panning else "grab"
        if st == STATE_SCROLL_OUT:
            return "zoom-in"
        return "default"

    # ═════════════════════════════════════════════════════════════════
    #  ZOOM & PAN
    # ═════════════════════════════════════════════════════════════════

    def reset_transform(self) -> None:
        self._scale = 1.0
        self._offset = QPointF(0, 0)
        self._apply_state()

    def _clamp(self, scale: float, off: QPointF) -> QPointF:
        if scale <= 1.001:
            return QPointF(0, 0)
        sw = self._surface.width() * scale
        sh = self._surface.height() * scale
        lx = max(0.0, (sw - self.width()) / 2 + 60)
        ly = max(0.0, (sh - self.height()) / 2 + 60)
        return QPointF(max(-lx, min(lx, off.x())), max(-ly, min(ly, off.y())))

    def zoom_at(self, target: float, anchor: QPointF | None = None) -> None:
        """Cursor-anchored zoom, clamped to ZOOM_MIN…ZOOM_MAX."""
        if anchor is None:
            anchor = QPointF(self.width() / 2, self.height() / 2)
        s0, o0 = self._scale, self._offset
        s1 = max(ZOOM_MIN, min(ZOOM_MAX, target))
        cx = anchor.x() - self.width() / 2
        cy = anchor.y() - self.height() / 2
        px = (cx - o0.x()) / s0 if s0 else 0.0
        py = (cy - o0.y()) / s0 if s0 else 0.0
        self._offset = self._clamp(s1, QPointF(cx - px * s1, cy - py * s1))
        self._scale = s1
        self._apply_state()

    def zoom_by(self, factor: float, anchor: QPointF | None = None) -> None:
        self.zoom_at(self._scale * factor, anchor)

    def set_aspect_ratio(self, ratio: float) -> None:
        """Call with mpv's video-params/aspect."""
        if ratio and 0.05 < ratio < 20:
            self._aspect = float(ratio)
            self._layout()

    # ═════════════════════════════════════════════════════════════════
    #  BRIDGE: host → shell slots
    # ═════════════════════════════════════════════════════════════════

    def set_media(
        self, path: str, requester: str = "", origin: str = "", quality: str = ""
    ) -> None:
        self._path = path
        self._has_media = bool(path)
        if requester:
            self._requester = requester
        if origin:
            self._origin = origin
        if quality:
            self.set_quality(quality)

        name = Path(path).name if path else "no media loaded"
        self._lbl_file.setText(name)
        self._lbl_requester.setText(f"Requested by: {self._requester}")
        self._lbl_origin.setText(f"{self._origin} · {time.strftime('%d.%m.%Y o %H:%M')}")
        self.setWindowTitle(f"{name} · Media Lightbox")
        self._position = 0.0
        self._layout()

    def set_duration(self, seconds: float) -> None:
        self._duration = max(0.0, float(seconds or 0))
        self._scrub.set_duration(self._duration)
        self._update_time_labels()

    def set_position(self, seconds: float) -> None:
        self._demo = False
        self._demo_timer.stop()
        self._position = max(0.0, float(seconds or 0))
        self._update_time_labels()

    def set_playing(self, playing: bool) -> None:
        self._playing = bool(playing)
        self._btn_play.set_icon(Icon.pause if playing else Icon.play)
        self._set_center_glyph(None if playing else "play")
        if self._demo and playing:
            self._demo_timer.start()

    def set_volume(self, v: float, from_host: bool = False) -> None:
        self._volume = max(0.0, min(1.0, float(v)))
        self._vol.set_volume(self._volume)
        if self._volume > 0 and self._muted:
            self._muted = False
            self._vol.set_muted(False)
        if from_host:
            self._toast.flash(f"Volume {int(self._volume * 100)} %")

    def set_muted(self, m: bool) -> None:
        self._muted = bool(m)
        self._vol.set_muted(m)

    def set_rate(self, r: float) -> None:
        self._rate = max(0.25, min(2.0, float(r)))
        self._flyout.set_rate(self._rate)

    def set_quality(self, q: str) -> None:
        self._quality = q or "HD"
        self._flyout.set_quality(self._quality)

    def video_surface(self) -> QWidget:
        """The widget mpv should render into (use .winId())."""
        return self._surface

    # ═════════════════════════════════════════════════════════════════
    #  INTERNAL CONTROLS
    # ═════════════════════════════════════════════════════════════════

    def _update_time_labels(self) -> None:
        self._lbl_elapsed.setText(fmt_time(self._position))
        self._lbl_remaining.setText(fmt_signed(max(0.0, self._duration - self._position)))
        if self._duration > 0 and not self._scrub.is_dragging():
            self._scrub.set_value_silent(self._position / self._duration)
        self._refresh_hud()

    def _set_center_glyph(self, kind) -> None:
        self._center.set_kind(kind)

    def _on_play_clicked(self) -> None:
        if not self._has_media:
            self._toast.flash("No media loaded — drop a file to begin")
            return
        self.playPauseRequested.emit()
        if self._demo:
            self.set_playing(not self._playing)

    def _on_scrub(self, frac: float) -> None:
        t = frac * self._duration
        self._position = t
        self._lbl_elapsed.setText(fmt_time(t))
        self._lbl_remaining.setText(fmt_signed(max(0.0, self._duration - t)))
        self.seekRequested.emit(t)

    def _on_volume(self, v: float) -> None:
        self.set_volume(v)
        self.volumeChanged.emit(self._volume)
        self._toast.flash(f"Volume {int(self._volume * 100)} %")

    def _toggle_mute(self) -> None:
        self._muted = not self._muted
        self._vol.set_muted(self._muted)
        self.muteChanged.emit(self._muted)
        self._toast.flash("Muted" if self._muted else f"Volume {int(self._volume * 100)} %")

    def _on_rate(self, r: float) -> None:
        self.set_rate(r)
        self.rateChanged.emit(r)
        self._toast.flash(f"Speed {r:g}x")

    def _toggle_window_mode(self) -> None:
        self._rounded = not self._rounded
        self._apply_state()

    def toggle_hud(self) -> None:
        self._show_hud = not self._show_hud
        self._btn_hud.set_tint(C_ACCENT if self._show_hud else None)
        self._apply_state()

    def _open_settings(self) -> None:
        self._flyout.set_rate(self._rate)
        self._flyout.set_quality(self._quality)
        self._flyout.popup_near(
            self._btn_settings.mapToGlobal(QPoint(self._btn_settings.width(), 0))
        )

    def _open_keys(self) -> None:
        self._keys.popup_near(self._btn_keys.mapToGlobal(QPoint(0, 0)))

    def _open_menu(self) -> None:
        m = QMenu(self)
        m.setFont(ui(10))
        m.setStyleSheet(
            "QMenu { background: rgba(35,39,45,246); color: rgba(255,255,255,200);"
            "  border: 1px solid rgba(255,255,255,30); border-radius: 8px;"
            "  padding: 6px; }"
            "QMenu::item { padding: 6px 22px; border-radius: 5px; }"
            "QMenu::item:selected { background: rgba(255,255,255,26); }"
            "QMenu::separator { height: 1px; background: rgba(255,255,255,25);"
            "  margin: 4px 8px; }"
        )
        m.addAction("Копіювати посилання", lambda: self._toast.flash("URL copied"))
        m.addAction("Відкрити у папці", lambda: self._toast.flash(self._path or "—"))
        m.addAction("Скинути масштаб  (R)", self.reset_transform)

        m.addSeparator()
        m.addAction("Вихід  (Esc)", self._exit)
        m.exec(self._btn_more.mapToGlobal(QPoint(0, self._btn_more.height())))

    def _exit(self) -> None:
        self._demo_timer.stop()
        self._refresh_timer.stop()
        self.closeRequested.emit()
        self.close()

    # ═════════════════════════════════════════════════════════════════
    #  DEMO CLOCK (standalone only)
    # ═════════════════════════════════════════════════════════════════

    def _demo_tick(self) -> None:
        if not self._playing or self._duration <= 0:
            return
        self._position = min(self._duration, self._position + 0.2 * self._rate)
        self._update_time_labels()
        if self._position >= self._duration:
            self._demo_timer.stop()
            self.set_playing(False)

    # ═════════════════════════════════════════════════════════════════
    #  KEYBOARD
    # ═════════════════════════════════════════════════════════════════

    def keyPressEvent(self, e) -> None:
        self._wake()
        k = e.key()

        if k == Qt.Key.Key_Space:
            e.accept()
            self._on_play_clicked()
        elif k in (Qt.Key.Key_F, Qt.Key.Key_F11):
            e.accept()
            self._toggle_fullscreen()
        elif k == Qt.Key.Key_M:
            e.accept()
            self._toggle_mute()
        elif k == Qt.Key.Key_Escape:
            e.accept()
            if self._flyout.isVisible():
                self._flyout.close()
            elif self._keys.isVisible():
                self._keys.close()
            elif self._fullscreen:
                self._exit_fullscreen()
            elif abs(self._scale - 1.0) > 0.005:
                self.reset_transform()
            else:
                self._exit()
        elif k == Qt.Key.Key_Right:
            e.accept()
            if self._duration:
                self.seekRequested.emit(min(self._duration, self._position + 5))
                self._on_scrub(self._scrub.value() + 5 / max(1, self._duration))
            else:
                self.nextRequested.emit()
        elif k == Qt.Key.Key_Left:
            e.accept()
            if self._duration:
                self.seekRequested.emit(max(0.0, self._position - 5))
                self._on_scrub(max(0.0, self._scrub.value() - 5 / max(1, self._duration)))
            else:
                self.prevRequested.emit()
        elif k == Qt.Key.Key_Up:
            e.accept()
            v = min(1.0, self._volume + 0.1)
            self.set_volume(v)
            self.volumeChanged.emit(v)
            self._toast.flash(f"Volume {int(v * 100)} %")
        elif k == Qt.Key.Key_Down:
            e.accept()
            v = max(0.0, self._volume - 0.1)
            self.set_volume(v)
            self.volumeChanged.emit(v)
            self._toast.flash(f"Volume {int(v * 100)} %")
        elif k in (Qt.Key.Key_Plus, Qt.Key.Key_Equal):
            e.accept()
            self.zoom_by(1 + ZOOM_STEP)
        elif k == Qt.Key.Key_Minus:
            e.accept()
            self.zoom_by(1 - ZOOM_STEP)
        elif k in (Qt.Key.Key_R, Qt.Key.Key_0):
            e.accept()
            self.reset_transform()
        elif k == Qt.Key.Key_D:
            e.accept()
            self.toggle_hud()
        elif k in (Qt.Key.Key_Question, Qt.Key.Key_Slash):
            e.accept()
            self._open_keys()
        else:
            super().keyPressEvent(e)

    # ═════════════════════════════════════════════════════════════════
    #  POINTER / WHEEL / GESTURES
    # ═════════════════════════════════════════════════════════════════

    def wheelEvent(self, e) -> None:
        self._wake()
        dy = e.angleDelta().y()
        if dy == 0:
            return
        self.zoom_by(1 + ZOOM_STEP * (1 if dy > 0 else -1), QPointF(e.position()))

    def mouseDoubleClickEvent(self, e) -> None:
        if e.button() == Qt.MouseButton.LeftButton:
            if self._scale > 1.02:
                self.reset_transform()
            else:
                self.zoom_at(ZOOM_DOUBLE_CLICK, QPointF(e.position()))

    def _hit_edges(self, pos: QPointF) -> tuple[bool, bool, bool, bool]:
        if not self._frameless or self._fullscreen:
            return (False, False, False, False)
        g = EDGE_GRIP
        return (pos.x() < g, pos.y() < g, pos.x() > self.width() - g, pos.y() > self.height() - g)

    def mousePressEvent(self, e) -> None:
        self._wake()
        if e.button() != Qt.MouseButton.LeftButton:
            return

        edges = self._hit_edges(e.position())
        if any(edges):
            self._resizing = edges
            self._drag_geom = self.geometry()
            self._drag_origin = e.globalPosition().toPoint()
            self._grab()
            return

        if self._frameless and self._scale <= 1.001 and self._over_backdrop(e.position()):
            self._dragging_window = True
            self._drag_geom = self.geometry()
            self._drag_origin = e.globalPosition().toPoint()
            self._grab()
            return

        self._press = QPointF(e.position())
        self._press_offset = QPointF(self._offset)
        self._moved = False
        self._panning = False
        self._grab()

    def _over_backdrop(self, pos: QPointF) -> bool:
        g = self._surface.geometry()
        return not g.contains(pos.toPoint())

    def mouseMoveEvent(self, e) -> None:
        if self._fullscreen:
            self._wake()

        # ── frameless resize ────────────────────────────────────────
        if any(self._resizing):
            l, t, r, b = self._resizing
            d = e.globalPosition().toPoint() - self._drag_origin
            g = QRect(self._drag_geom)
            if r:
                g.setWidth(max(MIN_WINDOW.width(), g.width() + d.x()))
            if b:
                g.setHeight(max(MIN_WINDOW.height(), g.height() + d.y()))
            if l:
                nx = min(self._drag_geom.right() - MIN_WINDOW.width(), self._drag_geom.x() + d.x())
                g.setLeft(nx)
            if t:
                ny = min(
                    self._drag_geom.bottom() - MIN_WINDOW.height(), self._drag_geom.y() + d.y()
                )
                g.setTop(ny)
            self.setGeometry(g)
            return

        # ── window move ─────────────────────────────────────────────
        if self._dragging_window:
            d = e.globalPosition().toPoint() - self._drag_origin
            self.move(self._drag_geom.topLeft() + d)
            return

        # ── edge cursor feedback (never clobbers the state cursor) ──
        if not (e.buttons() & Qt.MouseButton.LeftButton):
            if not self._hud_hidden:
                l, t, r, b = self._hit_edges(e.position())
                if any((l, t, r, b)):
                    if (l and t) or (r and b):
                        shape = Qt.CursorShape.SizeFDiagCursor
                    elif (l and b) or (r and t):
                        shape = Qt.CursorShape.SizeBDiagCursor
                    elif l or r:
                        shape = Qt.CursorShape.SizeHorCursor
                    else:
                        shape = Qt.CursorShape.SizeVerCursor
                    if self.cursor().shape() != shape:
                        self.setCursor(shape)
                    return
                self._set_state_cursor()
            return

        # ── pan (ZOOM_IN_PAN only) ─────────────────────────────────
        if not (e.buttons() & Qt.MouseButton.LeftButton):
            return
        dx = e.position().x() - self._press.x()
        dy = e.position().y() - self._press.y()
        if not self._moved and math.hypot(dx, dy) < DRAG_THRESHOLD_PX:
            return
        self._moved = True
        if self._scale <= 1.001:
            return  # pan inactive at fit scale
        self._panning = True
        self._offset = self._clamp(self._scale, self._press_offset + QPointF(dx, dy))
        self._layout()

    def _grab(self) -> None:
        """Hold the pointer so a drag survives the cursor leaving the window."""
        if not self._grabbed:
            self._grabbed = True
            self.grabMouse()

    def _release(self) -> None:
        if self._grabbed:
            self._grabbed = False
            self.releaseMouse()

    def mouseReleaseEvent(self, e) -> None:
        self._release()
        if any(self._resizing):
            self._resizing = (False, False, False, False)
            self._set_state_cursor()
            return
        if self._dragging_window:
            self._dragging_window = False
            self._set_state_cursor()
            return
        was_panning = self._panning
        self._panning = False
        if was_panning:
            self._apply_state()
        else:
            self._set_state_cursor()

    def eventFilter(self, obj, ev) -> bool:
        """Forward canvas-surface input to the shell."""
        if obj is self._surface:
            t = ev.type()
            if t in (
                QEvent.Type.MouseButtonPress,
                QEvent.Type.MouseButtonDblClick,
                QEvent.Type.MouseMove,
                QEvent.Type.MouseButtonRelease,
                QEvent.Type.Wheel,
            ):
                # re-target into shell coordinates
                gl = (
                    ev.globalPosition().toPoint()
                    if hasattr(ev, "globalPosition")
                    else ev.globalPos()
                )
                local = self.mapFromGlobal(gl)
                ev.setAccepted(False)
                if t == QEvent.Type.MouseButtonPress:
                    self.mousePressEvent(_ReposedEvent(ev, local))
                elif t == QEvent.Type.MouseButtonDblClick:
                    self.mouseDoubleClickEvent(_ReposedEvent(ev, local))
                elif t == QEvent.Type.MouseMove:
                    self.mouseMoveEvent(_ReposedEvent(ev, local))
                elif t == QEvent.Type.MouseButtonRelease:
                    self.mouseReleaseEvent(_ReposedEvent(ev, local))
                elif t == QEvent.Type.Wheel:
                    self.wheelEvent(_ReposedEvent(ev, local))
                return True
        return super().eventFilter(obj, ev)

    # ── drag & drop ─────────────────────────────────────────────────

    def dragEnterEvent(self, e) -> None:
        if e.mimeData().hasUrls():
            e.acceptProposedAction()

    def dropEvent(self, e) -> None:
        for url in e.mimeData().urls():
            p = url.toLocalFile()
            if p.lower().endswith((".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v", ".ts", ".flv")):
                self.mediaDropped.emit(p)
                e.acceptProposedAction()
                return

    # ═════════════════════════════════════════════════════════════════
    #  PAINT & LIFECYCLE
    # ═════════════════════════════════════════════════════════════════

    def paintEvent(self, _) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)

        if self._fullscreen:
            p.fillRect(self.rect(), C_BG_0)
        else:
            g = QRadialGradient(
                QPointF(self.width() / 2, self.height() * 0.40),
                max(self.width(), self.height()) * 0.75,
            )
            g.setColorAt(0.0, QColor(5, 6, 10, 165))
            g.setColorAt(1.0, QColor(5, 6, 10, 250))
            p.fillRect(self.rect(), QBrush(g))

        # chevron discs + HD gear badge are painted under their buttons
        if self._btn_prev.isVisible():
            for btn in (self._btn_prev, self._btn_next):
                rr(p, QRectF(btn.geometry()).adjusted(0, 0, 0, 0), 21, QColor(0, 0, 0, 110), W_15)

        # floating overlays while zoomed (manifest: FLOATING_OVERLAY)
        if self.state() == STATE_ZOOM_IN_PAN and not self._hud_hidden:
            f = mono(8)
            txt = "ZOOM_IN_PAN · drag to pan · double-click to reset"
            tw = text_width(f, txt) + 22
            x = (self.width() - tw) / 2
            y = self.height() * 0.055
            rr(p, QRectF(x, y, tw, 20), 10, QColor(0, 0, 0, 135))
            p.setFont(f)
            p.setPen(W_55)
            p.drawText(QRectF(x, y, tw, 20), Qt.AlignmentFlag.AlignCenter, txt)

        # empty-state hint
        if not self._has_media and not self._hud_hidden:
            f = ui(11)
            lines = ["no media loaded", "drop a video file here  ·  or  python shell.py <file>"]
            p.setFont(f)
            for i, ln in enumerate(lines):
                p.setPen(W_55 if i == 0 else W_35)
                p.drawText(
                    QRectF(0, self.height() / 2 - 30 + i * 22, self.width(), 22),
                    Qt.AlignmentFlag.AlignHCenter,
                    ln,
                )
        p.end()

    def showEvent(self, e) -> None:
        super().showEvent(e)
        self._apply_state()
        self._layout()
        self._refresh_timer.start()
        if self._demo and self._playing:
            self._demo_timer.start()

    def closeEvent(self, e) -> None:
        self._release()
        self._panning = False
        self._resizing = (False, False, False, False)
        self._dragging_window = False
        self._demo_timer.stop()
        self._refresh_timer.stop()
        self._hide_timer.stop()
        self._flyout.close()
        self._keys.close()
        super().closeEvent(e)


# ═══════════════════════════════════════════════════════════════════════════════
#  Helper: re-target a Qt mouse/wheel event onto the shell's own coordinates
# ═══════════════════════════════════════════════════════════════════════════════


class _ReposedEvent:
    """Lightweight shim exposing position()/globalPosition()/buttons() etc.
    for an event that originated on a child widget."""

    __slots__ = ("_e", "_local")

    def __init__(self, e, local: QPoint):
        self._e = e
        self._local = local

    # mouse
    def position(self) -> QPointF:
        return QPointF(self._local)

    def globalPosition(self) -> QPointF:
        return QPointF(self._e.globalPosition())

    def button(self):
        return self._e.button()

    def buttons(self):
        return self._e.buttons()

    def modifiers(self):
        return self._e.modifiers()

    # wheel
    def angleDelta(self):
        return self._e.angleDelta()

    # generic
    def accept(self):
        return self._e.accept()

    def ignore(self):
        return self._e.ignore()

    def setAccepted(self, v):
        return self._e.setAccepted(v)

    def type(self):
        return self._e.type()


# ═══════════════════════════════════════════════════════════════════════════════
#  STANDALONE ENTRY
# ═══════════════════════════════════════════════════════════════════════════════


def _apply_app_style(app: QApplication) -> None:
    """Standalone-only: sets an APPLICATION stylesheet.

    mikai must never call this. It would restyle every window in the app, not
    just the player, and mikai themes itself through mikai_ui.theme. The shell
    paints its own chrome with QPainter, so it needs nothing from here beyond
    the Fusion style mikai_ui.main already applies.
    """
    app.setStyle("Fusion")
    app.setStyleSheet(
        """
        QWidget { font-family: '%s'; font-size: 12px; color: #e7e9ee; }
        QToolTip {
            background: rgba(24,25,28,235); color: rgba(255,255,255,215);
            border: 1px solid rgba(255,255,255,32); border-radius: 5px;
            padding: 4px 8px; font-size: 11px;
        }
        QPushButton {
            background: transparent; border: none; color: #e7e9ee;
        }
        QMenu { font-size: 12px; }
        """
        % _SYS_UI
    )


def main(argv: list[str]) -> int:
    app = QApplication(argv)
    app.setApplicationName("Enhanced Media Player Shell")
    _apply_app_style(app)

    shell = PlayerShell(demo_clock=True)

    arg_file = ""
    for a in argv[1:]:
        if not a.startswith("-") and Path(a).is_file():
            arg_file = a
            break

    if arg_file:
        shell.set_media(arg_file, "Dmitry", "UDream", "HD")
        shell.set_duration(35.0)  # mpv duration-changed replaces this
        shell.set_playing(True)
    else:
        shell.set_media("", "Dmitry", "UDream", "HD")
        shell.set_duration(0.0)

    # demo wiring: the host would do this against mpv instead
    shell.mediaDropped.connect(
        lambda p: (
            shell.set_media(p, "Dmitry", "UDream", "HD"),
            shell.set_duration(35.0),
            shell.set_playing(True),
        )
    )
    shell.stateChanged.connect(lambda s: print(f"[state] {s}"))

    shell.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
