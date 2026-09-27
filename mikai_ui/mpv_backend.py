"""Event-driven adapter around python-mpv.

Only this module imports python-mpv or reads mpv properties. The controller
consumes its Qt signals and exposes application-level playback state.
"""

from __future__ import annotations

import ctypes.util
import logging
import sys
from pathlib import Path
from typing import Any

from PySide6.QtCore import QObject, Signal

from mikai_backend import mpv_runtime

log = logging.getLogger(__name__)


def embedded_mpv_options(wid: str) -> dict[str, Any]:
    """Configure mpv for an aspect-correct, keyboard-neutral Qt embed."""
    return {
        "wid": wid,
        "input_default_bindings": False,
        "input_vo_keyboard": False,
        "input_cursor": False,
        "osc": False,
        "hwdec": "auto-safe",
        "keepaspect": True,
        "volume": 100,
    }


def prepare_runtime() -> Path | None:
    """Find or prepare libmpv without imposing Windows on Qt fullscreen."""
    if sys.platform == "win32":
        return mpv_runtime.ensure_runtime()
    if ctypes.util.find_library("mpv"):
        return None
    raise FileNotFoundError("libmpv is not installed. Install the system mpv development library.")


class MPVAdapter(QObject):
    """Small, widget-agnostic mpv boundary with queued Qt event signals."""

    positionChanged = Signal(float)
    durationChanged = Signal(float)
    stateChanged = Signal(str)
    bufferingChanged = Signal(bool)
    mediaEnded = Signal()
    errorOccurred = Signal(str)
    mediaLoaded = Signal()
    volumeChanged = Signal(float)
    muteChanged = Signal(bool)
    speedChanged = Signal(float)

    def __init__(
        self,
        video_widget: Any,
        mpv_path: str | Path | None = None,
        *,
        auto_download: bool = True,
    ) -> None:
        super().__init__()
        self._player: Any | None = None
        self._dll_directory_handles: list[Any] = []
        self._mpv_library: Any | None = None
        self._closed = False
        self._position = 0.0
        self._duration = 0.0
        self._paused = False
        self._buffering = False
        self._volume = 100.0
        self._muted = False
        self._speed = 1.0

        try:
            runtime_dir = self._resolve_runtime(mpv_path, auto_download)
            if runtime_dir is not None:
                self._dll_directory_handles, self._mpv_library = mpv_runtime.configure_runtime(
                    runtime_dir
                )
            self._initialize(video_widget)
        except Exception as exc:
            log.exception("could not initialize embedded mpv")
            self.errorOccurred.emit(str(exc))
            self.close()
            raise

    @staticmethod
    def _resolve_runtime(mpv_path: str | Path | None, auto_download: bool) -> Path | None:
        if sys.platform == "win32":
            return mpv_runtime.ensure_runtime(mpv_path, auto_download=auto_download)
        if mpv_path is not None:
            return mpv_runtime.find_runtime(mpv_path)
        if ctypes.util.find_library("mpv"):
            return None
        raise FileNotFoundError(
            "libmpv is not installed. Install the system mpv development library."
        )

    def _initialize(self, video_widget: Any) -> None:
        import mpv

        self._player = mpv.MPV(**embedded_mpv_options(str(int(video_widget.winId()))))
        self._register_observers()
        self._register_events()

    def _register_observers(self) -> None:
        player = self._player
        if player is None:
            return

        @player.property_observer("time-pos")
        def on_position(_name: str, value: Any) -> None:
            self._position = self._number(value)
            self.positionChanged.emit(self._position)

        @player.property_observer("duration")
        def on_duration(_name: str, value: Any) -> None:
            self._duration = self._number(value)
            self.durationChanged.emit(self._duration)

        @player.property_observer("pause")
        def on_pause(_name: str, value: Any) -> None:
            self._paused = bool(value)
            self.stateChanged.emit("paused" if self._paused else "playing")

        @player.property_observer("volume")
        def on_volume(_name: str, value: Any) -> None:
            self._volume = self._number(value, self._volume)
            self.volumeChanged.emit(self._volume)

        @player.property_observer("mute")
        def on_mute(_name: str, value: Any) -> None:
            self._muted = bool(value)
            self.muteChanged.emit(self._muted)

        @player.property_observer("speed")
        def on_speed(_name: str, value: Any) -> None:
            self._speed = self._number(value, self._speed)
            self.speedChanged.emit(self._speed)

        @player.property_observer("paused-for-cache")
        def on_cache_pause(_name: str, value: Any) -> None:
            self._set_buffering(bool(value))

    def _register_events(self) -> None:
        player = self._player
        if player is None:
            return

        @player.event_callback("start-file")
        def on_start_file(_event: Any) -> None:
            self._set_buffering(False)
            self.stateChanged.emit("loading")

        @player.event_callback("file-loaded")
        def on_file_loaded(_event: Any) -> None:
            self._duration = self._read_number("duration")
            self.durationChanged.emit(self._duration)
            self.mediaLoaded.emit()

        @player.event_callback("playback-restart")
        def on_playback_restart(_event: Any) -> None:
            self._set_buffering(False)
            self.stateChanged.emit("paused" if self._paused else "playing")

        @player.event_callback("end-file")
        def on_end_file(event: Any) -> None:
            reason = self._event_value(event, "reason")
            if reason == "error":
                detail = self._event_value(event, "error") or "mpv end-file error"
                self.errorOccurred.emit(str(detail))
                self.stateChanged.emit("error")
            elif reason == "eof":
                self.stateChanged.emit("finished")
                self.mediaEnded.emit()
            elif reason == "stop":
                self.stateChanged.emit("stopped")

    @staticmethod
    def _event_value(event: Any, key: str) -> Any:
        try:
            return event.get(key)
        except AttributeError, TypeError:
            return getattr(event, key, None)

    @staticmethod
    def _number(value: Any, fallback: float = 0.0) -> float:
        try:
            parsed = float(value)
        except TypeError, ValueError:
            return fallback
        return parsed if parsed >= 0 else fallback

    def _read_number(self, prop: str) -> float:
        player = self._player
        if player is None:
            return 0.0
        try:
            return self._number(getattr(player, prop.replace("-", "_")))
        except Exception:  # noqa: BLE001 - mpv property reads can raise FFI-backed errors.
            return 0.0

    def _set_buffering(self, buffering: bool) -> None:
        if self._buffering == buffering:
            return
        self._buffering = buffering
        self.bufferingChanged.emit(buffering)
        if buffering:
            self.stateChanged.emit("buffering")

    def _command(self, label: str, operation: Any) -> bool:
        if self._closed or self._player is None:
            return False
        try:
            operation(self._player)
        except Exception as exc:
            log.exception("mpv %s command failed", label)
            self.errorOccurred.emit(str(exc))
            return False
        return True

    def load(self, url: str | Path) -> None:
        self._set_buffering(False)
        self.stateChanged.emit("loading")
        self._command("load", lambda player: player.play(str(url)))

    def play(self) -> None:
        self.set_paused(False)

    def set_paused(self, paused: bool) -> None:
        self._paused = bool(paused)
        self._command("pause", lambda player: setattr(player, "pause", self._paused))

    def toggle_pause(self) -> None:
        self.set_paused(not self._paused)

    def seek(self, seconds: float) -> None:
        target = max(0.0, float(seconds))
        if self._duration > 0:
            target = min(target, self._duration)
        self._command(
            "absolute seek",
            lambda player: player.seek(target, reference="absolute", precision="exact"),
        )

    def set_volume(self, value: float) -> None:
        self._volume = max(0.0, min(100.0, float(value)))
        self._command("volume", lambda player: setattr(player, "volume", self._volume))

    def set_muted(self, muted: bool) -> None:
        self._muted = bool(muted)
        self._command("mute", lambda player: setattr(player, "mute", self._muted))

    def set_speed(self, speed: float) -> None:
        self._speed = max(0.25, min(4.0, float(speed)))
        self._command("speed", lambda player: setattr(player, "speed", self._speed))

    def _close_player(self) -> None:
        if self._player is not None:
            player, self._player = self._player, None
            try:
                player.terminate()
            except Exception:
                log.debug("mpv terminate raised during shutdown", exc_info=True)

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._close_player()
        self._mpv_library = None
        for handle in self._dll_directory_handles:
            try:
                handle.close()
            except Exception:
                log.debug("DLL directory handle close failed", exc_info=True)
        self._dll_directory_handles.clear()
