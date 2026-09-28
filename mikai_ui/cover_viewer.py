"""Full-screen cover viewer: a sharp image floating over a blurred copy of itself.

Deliberately chrome-free — no title bar, no buttons, no scrollbars. Telegram
shows a picture the same way, and every piece of furniture here competes with
the one thing the overlay exists to show. A click on the empty background
dismisses it, the wheel zooms under the cursor, and dragging pans once zoomed
past the fit scale.
"""

from __future__ import annotations

import logging

from PySide6.QtCore import QPoint, QPointF, QRect, QSize, Qt, Signal
from PySide6.QtGui import (
    QColor,
    QImage,
    QKeySequence,
    QPainter,
    QPixmap,
    QShortcut,
    QWheelEvent,
)
from PySide6.QtWidgets import QApplication, QDialog, QLabel, QWidget

from mikai_ui.theme.metrics import (
    COVER_BACKDROP_DIM,
    COVER_BACKDROP_DOWNSCALE,
    COVER_ZOOM_MAX,
    COVER_ZOOM_MIN,
    COVER_ZOOM_STEP,
)

log = logging.getLogger(__name__)


class CoverLabel(QLabel):
    """Cover thumbnail that reports activation so the page can open it full size."""

    activated = Signal()

    def __init__(self, text: str, parent: QWidget | None = None) -> None:
        super().__init__(text, parent)
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._zoomable = False

    def set_zoomable(self, zoomable: bool) -> None:
        self._zoomable = zoomable
        self.setCursor(
            Qt.CursorShape.PointingHandCursor if zoomable else Qt.CursorShape.ArrowCursor
        )

    def mousePressEvent(self, event) -> None:  # noqa: N802 - Qt naming
        if self._zoomable and event.button() == Qt.MouseButton.LeftButton:
            self.activated.emit()
        super().mousePressEvent(event)


def blurred_backdrop(pixmap: QPixmap, size: QSize) -> QPixmap:
    """A frosted copy of the image, filled to the surface and heavily softened.

    A downscale-then-upscale pass is the whole trick: Qt's smooth scaler is a
    box filter, so two passes at different factors read as a real blur and cost
    two C++ calls instead of a per-pixel Python loop.
    """
    if pixmap.isNull() or size.isEmpty():
        return QPixmap()
    image = pixmap.toImage()
    if image.isNull():
        return QPixmap()
    if not image.hasAlphaChannel():
        image = image.convertToFormat(QImage.Format.Format_ARGB32_Premultiplied)
    for factor in (COVER_BACKDROP_DOWNSCALE, 2):
        small = image.scaled(
            QSize(
                max(1, size.width() // factor),
                max(1, size.height() // factor),
            ),
            Qt.AspectRatioMode.KeepAspectRatioByExpanding,
            Qt.TransformationMode.SmoothTransformation,
        )
        image = small.scaled(
            size,
            Qt.AspectRatioMode.KeepAspectRatioByExpanding,
            Qt.TransformationMode.SmoothTransformation,
        )
    # Deliberately not QPixmap.fill(): that rewrites every channel, alpha
    # included, so it erases the blur instead of dimming it and the overlay
    # ends up on a flat black field.
    return QPixmap.fromImage(image)


class CoverView(QWidget):
    """The image surface: blurred backdrop behind, sharp image in front."""

    zoomChanged = Signal(float)
    backgroundClicked = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("coverViewerView")
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setCursor(Qt.CursorShape.OpenHandCursor)

        self._image = QPixmap()
        self._backdrop = QPixmap()
        self._zoom = 1.0
        self._fit = 1.0
        self._pan = QPointF(0.0, 0.0)
        self._drag_origin: QPointF | None = None
        self._pan_origin = QPointF(0.0, 0.0)

    # -- content -------------------------------------------------------
    def set_image(self, pixmap: QPixmap) -> None:
        self._image = pixmap
        self._zoom = 1.0
        self._pan = QPointF(0.0, 0.0)
        self._recompute_fit()

    @property
    def scale(self) -> float:
        return self._fit * self._zoom

    @property
    def zoom(self) -> float:
        return self._zoom

    def _target_size(self) -> QSize:
        return QSize(
            max(1, round(self._image.width() * self.scale)),
            max(1, round(self._image.height() * self.scale)),
        )

    def _image_rect(self) -> QRect:
        size = self._target_size()
        centre = QPointF(self.width() / 2.0, self.height() / 2.0) + self._pan
        return QRect(
            QPoint(round(centre.x() - size.width() / 2), round(centre.y() - size.height() / 2)),
            size,
        )

    # -- zoom ----------------------------------------------------------
    def set_zoom(self, zoom: float) -> None:
        target = min(COVER_ZOOM_MAX, max(COVER_ZOOM_MIN, zoom))
        if abs(target - self._zoom) < 1e-6:
            return
        self._zoom = target
        if self._is_fitted():
            self._pan = QPointF(0.0, 0.0)
        self._clamp_pan()
        self.update()
        self.zoomChanged.emit(self.scale)

    def zoom_in(self) -> None:
        self.set_zoom(self._zoom * COVER_ZOOM_STEP)

    def zoom_out(self) -> None:
        self.set_zoom(self._zoom / COVER_ZOOM_STEP)

    def reset_zoom(self) -> None:
        self.set_zoom(1.0)

    def toggle_actual_size(self) -> None:
        """Flip between fitting the surface and showing the image at 1:1 pixels."""
        self.set_zoom(1.0 / self._fit if self._fit > 0 and self._zoom >= 1.0 else 1.0)

    def _is_fitted(self) -> bool:
        return abs(self._zoom - 1.0) < 1e-6

    def _compute_fit(self) -> float | None:
        if self._image.isNull() or self.width() <= 0 or self.height() <= 0:
            return None
        # Never upscale past native resolution just to fill the surface.
        return min(
            1.0,
            min(self.width() / self._image.width(), self.height() / self._image.height()),
        )

    def _recompute_fit(self) -> None:
        fit = self._compute_fit()
        if fit is None or abs(fit - self._fit) < 1e-9:
            return
        # Only reflow while fitted. Once zoomed the scale is the user's, and
        # re-fitting under them would fight that choice and never settle.
        if self._is_fitted():
            self._fit = fit
        self._clamp_pan()
        self._rebuild_backdrop()
        self.update()

    def _rebuild_backdrop(self) -> None:
        self._backdrop = blurred_backdrop(self._image, self.size())

    def _clamp_pan(self) -> None:
        """Keep the image from drifting entirely off the surface."""
        if self._is_fitted():
            self._pan = QPointF(0.0, 0.0)
            return
        rect = self._image_rect()
        slack_x = max(0.0, (rect.width() - self.width()) / 2.0)
        slack_y = max(0.0, (rect.height() - self.height()) / 2.0)
        self._pan.setX(min(slack_x, max(-slack_x, self._pan.x())))
        self._pan.setY(min(slack_y, max(-slack_y, self._pan.y())))

    # -- painting ------------------------------------------------------
    def paintEvent(self, event) -> None:  # noqa: N802 - Qt naming
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(0, 0, 0))
        if not self._backdrop.isNull() and self._backdrop.size() != self.size():
            self._backdrop = blurred_backdrop(self._image, self.size())
        if not self._backdrop.isNull():
            painter.drawPixmap(0, 0, self._backdrop)
            painter.fillRect(self.rect(), QColor(0, 0, 0, COVER_BACKDROP_DIM))
        if self._image.isNull():
            return
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
        painter.drawPixmap(self._image_rect(), self._image, QRect(self._image.rect()))
        painter.end()

    # -- input ---------------------------------------------------------
    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802 - Qt naming
        if self._image.isNull():
            super().wheelEvent(event)
            return
        delta = event.angleDelta().y()
        if not delta:
            super().wheelEvent(event)
            return
        self._zoom_at_cursor(COVER_ZOOM_STEP if delta > 0 else 1 / COVER_ZOOM_STEP, event)
        event.accept()

    def _zoom_at_cursor(self, factor: float, event: QWheelEvent) -> None:
        """Zoom while keeping the pixel under the pointer where it is."""
        anchor = event.position()
        before = self._image_rect()
        if before.isEmpty():
            return
        fx = (anchor.x() - before.left()) / max(1, before.width())
        fy = (anchor.y() - before.top()) / max(1, before.height())
        self.set_zoom(self._zoom * factor)
        after = self._image_rect()
        if after.isEmpty():
            return
        self._pan.setX(self._pan.x() + anchor.x() - (after.left() + fx * after.width()))
        self._pan.setY(self._pan.y() + anchor.y() - (after.top() + fy * after.height()))
        self._clamp_pan()
        self.update()

    def mousePressEvent(self, event) -> None:  # noqa: N802 - Qt naming
        if event.button() != Qt.MouseButton.LeftButton or self._image.isNull():
            super().mousePressEvent(event)
            return
        if not self._image_rect().contains(event.position().toPoint()):
            self.backgroundClicked.emit()
            event.accept()
            return
        self.setFocus(Qt.FocusReason.MouseFocusReason)
        self._drag_origin = event.position()
        self._pan_origin = QPointF(self._pan)
        self.setCursor(Qt.CursorShape.ClosedHandCursor)

    def mouseMoveEvent(self, event) -> None:  # noqa: N802 - Qt naming
        if self._drag_origin is None or self._is_fitted():
            return
        self._pan = QPointF(
            self._pan_origin.x() + event.position().x() - self._drag_origin.x(),
            self._pan_origin.y() + event.position().y() - self._drag_origin.y(),
        )
        self._clamp_pan()
        self.update()

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802 - Qt naming
        self._drag_origin = None
        self.setCursor(Qt.CursorShape.OpenHandCursor)

    def mouseDoubleClickEvent(self, event) -> None:  # noqa: N802 - Qt naming
        self.toggle_actual_size()
        event.accept()

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt naming
        super().resizeEvent(event)
        self._recompute_fit()

    def sizeHint(self) -> QSize:
        return self._target_size()


class CoverViewer(QDialog):
    """Frameless full-screen cover overlay, dismissed by a click on the backdrop."""

    def __init__(self, pixmap: QPixmap, caption: str = "", parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("coverViewer")
        self.setWindowTitle("Обкладинка" if not caption else f"Обкладинка — {caption}")
        self.setWindowFlag(Qt.WindowType.FramelessWindowHint, True)
        self.setWindowFlag(Qt.WindowType.Dialog, True)

        self._shortcuts: list[QShortcut] = []
        self._view = CoverView(self)
        self._view.set_image(pixmap)
        self._view.backgroundClicked.connect(self.close)
        # no layout: one canvas, pinned to the dialog, and nothing to arrange
        self._view.setGeometry(self.rect())

        for sequence, handler in (
            ("Ctrl++", self._view.zoom_in),
            ("Ctrl+=", self._view.zoom_in),
            ("Ctrl+-", self._view.zoom_out),
            ("Ctrl+0", self._view.reset_zoom),
            ("Esc", self.close),
        ):
            shortcut = QShortcut(QKeySequence(sequence), self)
            shortcut.setContext(Qt.ShortcutContext.WindowShortcut)
            shortcut.activated.connect(handler)
            self._shortcuts.append(shortcut)

    def showEvent(self, event) -> None:  # noqa: N802 - Qt naming
        super().showEvent(event)
        # Cover the whole SCREEN. parent.rect() is in the parent widget's own
        # coordinate space, but a top-level window reads setGeometry as screen
        # coordinates, so it used to pin the overlay to the screen origin at the
        # app's size: a fixed box in the corner instead of a full-screen viewer.
        parent = self.parentWidget()
        screen = (parent.screen() if parent is not None else None) or (QApplication.primaryScreen())
        if screen is not None:
            self.setGeometry(screen.availableGeometry())
        self._view.setGeometry(self.rect())
        self._view.setFocus(Qt.FocusReason.OtherFocusReason)

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt naming
        super().resizeEvent(event)
        self._view.setGeometry(self.rect())
