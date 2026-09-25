"""Cross-platform Qt fullscreen lifecycle and window restoration."""

from __future__ import annotations

from PySide6.QtCore import QObject, QRect, QTimer, Signal
from PySide6.QtGui import QScreen
from PySide6.QtWidgets import QWidget


class FullscreenManager(QObject):
    """Enter and leave native Qt fullscreen while restoring the prior window."""

    fullscreenChanged = Signal(bool)

    def __init__(self, window: QWidget) -> None:
        super().__init__(window)
        self._window = window
        self._saved_geometry: QRect | None = None
        self._saved_maximized = False
        self._active_screen: QScreen | None = None
        self._connected_window_handle = False
        QTimer.singleShot(0, self._watch_window_screen)

    @property
    def is_fullscreen(self) -> bool:
        return self._window.isFullScreen()

    def toggle(self) -> None:
        if self.is_fullscreen:
            self.exit()
        else:
            self.enter()

    def enter(self) -> None:
        if self.is_fullscreen:
            return
        self._saved_geometry = self._window.geometry()
        self._saved_maximized = self._window.isMaximized()
        handle = self._window.windowHandle()
        screen = handle.screen() if handle is not None else self._window.screen()
        self._set_active_screen(screen)
        self._window.showFullScreen()
        self.fullscreenChanged.emit(True)
        QTimer.singleShot(0, self._watch_window_screen)

    def exit(self) -> None:
        if not self.is_fullscreen:
            return
        self._window.showNormal()
        if self._saved_maximized:
            self._window.showMaximized()
        elif self._saved_geometry is not None:
            self._window.setGeometry(self._saved_geometry)
        self._set_active_screen(None)
        self.fullscreenChanged.emit(False)

    def _watch_window_screen(self) -> None:
        handle = self._window.windowHandle()
        if handle is None or self._connected_window_handle:
            return
        handle.screenChanged.connect(self._set_active_screen)
        self._connected_window_handle = True
        self._set_active_screen(handle.screen())

    def _set_active_screen(self, screen: QScreen | None) -> None:
        if self._active_screen is screen:
            return
        if self._active_screen is not None:
            try:
                self._active_screen.geometryChanged.disconnect(self._apply_screen_geometry)
            except RuntimeError:
                pass
        self._active_screen = screen
        if screen is not None:
            screen.geometryChanged.connect(self._apply_screen_geometry)
            if self.is_fullscreen:
                self._apply_screen_geometry(screen.geometry())

    def _apply_screen_geometry(self, geometry: QRect) -> None:
        if self.is_fullscreen:
            self._window.setGeometry(geometry)
