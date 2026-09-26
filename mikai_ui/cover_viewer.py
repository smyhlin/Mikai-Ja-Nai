"""Full-size cover viewer with zoom, opened by activating a title's cover."""

from __future__ import annotations

import logging

from PySide6.QtCore import QSize, Qt, Signal
from PySide6.QtGui import QKeySequence, QPixmap, QShortcut, QWheelEvent
from PySide6.QtWidgets import (
    QDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from mikai_ui.theme.metrics import (
    COVER_VIEWER_BAR_HEIGHT,
    COVER_VIEWER_DEFAULT_HEIGHT,
    COVER_VIEWER_DEFAULT_WIDTH,
    COVER_VIEWER_MIN_HEIGHT,
    COVER_VIEWER_MIN_WIDTH,
    COVER_ZOOM_MAX,
    COVER_ZOOM_MIN,
    COVER_ZOOM_STEP,
    ICON_SIZE,
    SPACING_MEDIUM,
    SPACING_SMALL,
)
from mikai_ui.theme.widgets import make_icon

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


class CoverView(QScrollArea):
    """Scrollable image surface with fit-relative zoom anchored at the cursor."""

    zoomChanged = Signal()

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWidgetResizable(False)
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setObjectName("coverViewerView")

        self._label = QLabel(self)
        self._label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setWidget(self._label)

        self._image = QPixmap()
        self._zoom = 1.0
        self._fit = 1.0

    def set_image(self, pixmap: QPixmap) -> None:
        self._image = pixmap
        self._zoom = 1.0
        self._recompute_fit()

    def zoom_in(self) -> None:
        self.set_zoom(self._zoom * COVER_ZOOM_STEP)

    def zoom_out(self) -> None:
        self.set_zoom(self._zoom / COVER_ZOOM_STEP)

    def reset_zoom(self) -> None:
        self.set_zoom(1.0)

    def toggle_actual_size(self) -> None:
        """Flip between fitting the surface and showing the image at 1:1 pixels."""
        if abs(self._zoom - 1.0) < 1e-6:
            self.set_zoom(1.0 / self._fit if self._fit > 0 else 1.0)
        else:
            self.set_zoom(1.0)

    def set_zoom(self, zoom: float) -> None:
        target = min(COVER_ZOOM_MAX, max(COVER_ZOOM_MIN, zoom))
        if abs(target - self._zoom) < 1e-6:
            return
        self._zoom = target
        self._apply()
        self.zoomChanged.emit()

    @property
    def effective_scale(self) -> float:
        return self._fit * self._zoom

    def _compute_fit(self) -> float | None:
        if self._image.isNull():
            return None
        viewport = self.viewport().size()
        if viewport.width() <= 0 or viewport.height() <= 0:
            return None
        # Never upscale past native resolution just to fill the surface.
        return min(
            1.0,
            min(
                viewport.width() / self._image.width(),
                viewport.height() / self._image.height(),
            ),
        )

    def _recompute_fit(self) -> None:
        self._refit()

    def _refit(self) -> None:
        fit = self._compute_fit()
        if fit is None or abs(fit - self._fit) < 1e-9:
            return
        # Only reflow while fitted. Once the user has zoomed, the scale is theirs:
        # recomputing the fit as scrollbars come and go would fight that choice and
        # never settle, so resizing leaves the zoom alone and "Вписати" re-fits.
        if abs(self._zoom - 1.0) < 1e-6:
            self._fit = fit
            self._apply()
            self.zoomChanged.emit()

    def _apply(self) -> None:
        if self._image.isNull():
            return
        scaled = self._image.scaled(
            self._image.size() * self.effective_scale,
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        self._label.setPixmap(scaled)
        self._label.resize(scaled.size())

    def _zoom_at(self, factor: float, anchor) -> None:
        old_width = max(1, self._label.width())
        old_height = max(1, self._label.height())
        hbar = self.horizontalScrollBar()
        vbar = self.verticalScrollBar()
        fraction_x = (hbar.value() + anchor.x()) / old_width
        fraction_y = (vbar.value() + anchor.y()) / old_height

        self.set_zoom(self._zoom * factor)

        hbar.setValue(int(fraction_x * self._label.width() - anchor.x()))
        vbar.setValue(int(fraction_y * self._label.height() - anchor.y()))

    def wheelEvent(self, event: QWheelEvent) -> None:  # noqa: N802 - Qt naming
        if not self._image.isNull():
            delta = event.angleDelta().y()
            if delta:
                factor = COVER_ZOOM_STEP if delta > 0 else 1 / COVER_ZOOM_STEP
                self._zoom_at(factor, event.position().toPoint())
                event.accept()
                return
        super().wheelEvent(event)

    def mouseDoubleClickEvent(self, event) -> None:  # noqa: N802 - Qt naming
        self.toggle_actual_size()
        super().mouseDoubleClickEvent(event)

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt naming
        super().resizeEvent(event)
        self._recompute_fit()

    def sizeHint(self) -> QSize:
        return self._label.size()


class CoverViewer(QDialog):
    """Modeless full-size cover window with zoom controls and a close action."""

    def __init__(self, pixmap: QPixmap, caption: str = "", parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("coverViewer")
        self.setWindowTitle("Обкладинка" if not caption else f"Обкладинка — {caption}")
        self.setMinimumSize(COVER_VIEWER_MIN_WIDTH, COVER_VIEWER_MIN_HEIGHT)
        self.resize(COVER_VIEWER_DEFAULT_WIDTH, COVER_VIEWER_DEFAULT_HEIGHT)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        self._view = CoverView(self)
        self._view.set_image(pixmap)
        root.addWidget(self._view, 1)

        bar = QWidget(self)
        bar.setObjectName("coverViewerBar")
        bar.setFixedHeight(COVER_VIEWER_BAR_HEIGHT)
        bar_layout = QHBoxLayout(bar)
        bar_layout.setContentsMargins(SPACING_MEDIUM, SPACING_SMALL, SPACING_MEDIUM, SPACING_SMALL)
        bar_layout.setSpacing(SPACING_SMALL)

        self._caption = QLabel(caption, bar)
        self._caption.setObjectName("muted")
        bar_layout.addWidget(self._caption, 1)

        self._zoom_label = QLabel("", bar)
        self._zoom_label.setObjectName("muted")
        self._zoom_label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
        bar_layout.addWidget(self._zoom_label)

        self._zoom_out_button = self._bar_button(bar, "zoom-out", "Зменшити")
        self._zoom_out_button.clicked.connect(self._view.zoom_out)
        bar_layout.addWidget(self._zoom_out_button)

        self._fit_button = QPushButton("Вписати", bar)
        self._fit_button.setToolTip("Показати зображення повністю")
        self._fit_button.clicked.connect(self._view.reset_zoom)
        bar_layout.addWidget(self._fit_button)

        self._zoom_in_button = self._bar_button(bar, "zoom-in", "Збільшити")
        self._zoom_in_button.clicked.connect(self._view.zoom_in)
        bar_layout.addWidget(self._zoom_in_button)

        close_button = self._bar_button(bar, "close", "Закрити (Esc)")
        close_button.clicked.connect(self.close)
        bar_layout.addWidget(close_button)

        root.addWidget(bar)

        self._view.zoomChanged.connect(self._update_zoom_label)
        self._update_zoom_label()

        for sequence, handler in (
            ("Ctrl++", self._view.zoom_in),
            ("Ctrl+=", self._view.zoom_in),
            ("Ctrl+-", self._view.zoom_out),
            ("Ctrl+0", self._view.reset_zoom),
            ("Esc", self.close),
        ):
            QShortcut(QKeySequence(sequence), self, handler)

    @staticmethod
    def _bar_button(parent: QWidget, icon: str, tooltip: str) -> QPushButton:
        button = QPushButton(parent)
        button.setIcon(make_icon(icon, size=ICON_SIZE))
        button.setToolTip(tooltip)
        button.setAccessibleName(tooltip)
        return button

    def _update_zoom_label(self) -> None:
        self._zoom_label.setText(f"{round(self._view.effective_scale * 100)}%")
