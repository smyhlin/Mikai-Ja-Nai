"""Playback commands and the single application-owned playback state."""

from __future__ import annotations

import logging
from dataclasses import dataclass, replace
from enum import Enum
from typing import Any, ClassVar

from PySide6.QtCore import QObject, QTimer, Signal, Slot

from mikai_ui.mpv_backend import MPVAdapter

log = logging.getLogger(__name__)


class PlaybackStatus(str, Enum):
    LOADING = "loading"
    PLAYING = "playing"
    PAUSED = "paused"
    BUFFERING = "buffering"
    FINISHED = "finished"
    ERROR = "error"
    STOPPED = "stopped"


@dataclass(frozen=True, slots=True)
class StreamInfo:
    """What mpv reports about the stream actually playing, not what we asked for."""

    width: int = 0
    height: int = 0
    fps: float = 0.0
    estimated_fps: float = 0.0

    @property
    def effective_fps(self) -> float:
        """The declared frame rate, or mpv's measured estimate when undeclared."""
        return self.fps if self.fps > 0 else self.estimated_fps

    def text(self) -> str:
        """Format as `<width>x<height> | <fps>fps`, dropping what is unknown."""
        parts: list[str] = []
        if self.width > 0 and self.height > 0:
            parts.append(f"{self.width}x{self.height}")
        fps = self.effective_fps
        if fps > 0:
            parts.append(f"{fps:.2f}".rstrip("0").rstrip(".") + "fps")
        return " | ".join(parts)


@dataclass(frozen=True, slots=True)
class PlaybackState:
    """Immutable snapshot consumed by controls and overlays."""

    status: PlaybackStatus = PlaybackStatus.LOADING
    position: float = 0.0
    duration: float = 0.0
    paused: bool = False
    volume: int = 100
    muted: bool = False
    speed: float = 1.0
    quality: int | None = None
    available: bool = False
    error: str | None = None
    stream: StreamInfo | None = None

    @property
    def playing(self) -> bool:
        return self.status is PlaybackStatus.PLAYING

    @property
    def buffering(self) -> bool:
        return self.status is PlaybackStatus.BUFFERING

    @property
    def loading(self) -> bool:
        return self.status is PlaybackStatus.LOADING

    @property
    def finished(self) -> bool:
        return self.status is PlaybackStatus.FINISHED


@dataclass(frozen=True, slots=True)
class _ResumePoint:
    position: float
    paused: bool
    volume: int
    muted: bool
    speed: float
    quality: int


class PlayerController(QObject):
    """Own player commands and translate adapter events into one state stream."""

    stateChanged = Signal(object)
    seekStarted = Signal()
    seekPreviewChanged = Signal(float)
    seekFinished = Signal(float)
    qualitySwitching = Signal(int)
    seekFeedback = Signal(str)

    _STATUS_BY_ADAPTER: ClassVar[dict[str, PlaybackStatus]] = {
        "loading": PlaybackStatus.LOADING,
        "playing": PlaybackStatus.PLAYING,
        "paused": PlaybackStatus.PAUSED,
        "buffering": PlaybackStatus.BUFFERING,
        "finished": PlaybackStatus.FINISHED,
        "error": PlaybackStatus.ERROR,
        "stopped": PlaybackStatus.STOPPED,
    }

    def __init__(
        self,
        url: str,
        qualities: dict[int, str] | None = None,
        initial_quality: int | None = None,
        parent: QObject | None = None,
    ) -> None:
        super().__init__(parent)
        self._initial_url = url
        self._current_url = url
        self._qualities = dict(qualities or {})
        self._adapter: MPVAdapter | Any | None = None
        self._state = PlaybackState(quality=initial_quality)
        self._closed = False
        self._has_loaded = False
        self._seek_origin_status = PlaybackStatus.LOADING
        self._seek_active = False
        self._resume_point: _ResumePoint | None = None
        self._load_watchdog = QTimer(self)
        self._load_watchdog.setSingleShot(True)
        self._load_watchdog.setInterval(30_000)
        self._load_watchdog.timeout.connect(self._on_load_timeout)

    @property
    def state(self) -> PlaybackState:
        return self._state

    @property
    def adapter(self) -> MPVAdapter | Any | None:
        """Return the adapter boundary, never the raw python-mpv player."""
        return self._adapter

    @property
    def current_url(self) -> str:
        return self._current_url

    def attach_adapter(self, adapter: MPVAdapter | Any) -> None:
        if self._closed:
            adapter.close()
            return
        if self._adapter is not None:
            self._adapter.close()
        self._adapter = adapter
        adapter.positionChanged.connect(self._on_position)
        adapter.durationChanged.connect(self._on_duration)
        adapter.stateChanged.connect(self._on_adapter_state)
        adapter.bufferingChanged.connect(self._on_buffering)
        adapter.mediaEnded.connect(self._on_media_ended)
        adapter.errorOccurred.connect(self._on_error)
        adapter.mediaLoaded.connect(self._on_media_loaded)
        adapter.volumeChanged.connect(self._on_volume)
        adapter.muteChanged.connect(self._on_muted)
        adapter.speedChanged.connect(self._on_speed)
        adapter.streamInfoChanged.connect(self._on_stream_info)
        self._set_state(available=True, status=PlaybackStatus.LOADING, error=None)

    def _set_state(self, **changes: Any) -> None:
        next_state = replace(self._state, **changes)
        if next_state == self._state:
            return
        self._state = next_state
        self.stateChanged.emit(next_state)

    def load(self) -> None:
        adapter = self._adapter
        if adapter is None or self._closed:
            return
        self._current_url = self._initial_url
        self._resume_point = None
        self._has_loaded = False
        self._set_state(status=PlaybackStatus.LOADING, error=None, position=0.0)
        self._load_watchdog.start()
        adapter.load(self._current_url)

    def replace_stream(self, url: str, qualities: dict[int, str]) -> None:
        """Point the player at a freshly extracted stream after a failed load."""
        if self._closed or not url:
            return
        self._qualities = dict(qualities)
        self._initial_url = url
        self._current_url = url
        self._resume_point = None
        self._has_loaded = False
        self._set_state(status=PlaybackStatus.LOADING, error=None, position=0.0)
        if self._adapter is None:
            return
        self._load_watchdog.start()
        self._adapter.load(url)

    def play(self) -> None:
        if self._adapter is None or self._closed:
            return
        if self._state.status in (PlaybackStatus.LOADING, PlaybackStatus.ERROR):
            return
        if self._state.status is PlaybackStatus.STOPPED:
            self._has_loaded = False
            self._set_state(status=PlaybackStatus.LOADING, error=None)
            self._load_watchdog.start()
            self._adapter.load(self._current_url or self._initial_url)
            return
        if self._state.status is PlaybackStatus.FINISHED:
            self.seek(0.0)
        self._adapter.play()
        self._set_state(status=PlaybackStatus.PLAYING, paused=False, error=None)

    def pause(self) -> None:
        if (
            self._adapter is None
            or self._closed
            or self._state.status
            in (PlaybackStatus.LOADING, PlaybackStatus.ERROR, PlaybackStatus.STOPPED)
        ):
            return
        self._adapter.set_paused(True)
        self._set_state(status=PlaybackStatus.PAUSED, paused=True)

    def toggle_play_pause(self) -> None:
        if self._state.status in (PlaybackStatus.LOADING, PlaybackStatus.ERROR):
            return
        if self._state.paused or self._state.status is PlaybackStatus.STOPPED:
            self.play()
        else:
            self.pause()

    def begin_seek(self) -> None:
        if not self._can_seek():
            return
        self._seek_origin_status = self._state.status
        self._seek_active = True
        self.seekStarted.emit()

    def preview_seek(self, target: float) -> float:
        if not self._can_seek():
            return self._state.position
        position = self._clamp_position(target)
        self._set_state(position=position)
        self.seekPreviewChanged.emit(position)
        return position

    def finish_seek(self, target: float) -> float:
        if not self._can_seek():
            self._seek_active = False
            return self._state.position
        position = self._clamp_position(target)
        if self._adapter is not None and not self._closed:
            self._adapter.seek(position)
        status = self._seek_origin_status if self._seek_active else self._state.status
        self._seek_active = False
        if status in (PlaybackStatus.FINISHED, PlaybackStatus.STOPPED):
            status = PlaybackStatus.PAUSED
        self._set_state(position=position, status=status, error=None)
        self.seekFinished.emit(position)
        return position

    def seek(self, target: float) -> float:
        return self.finish_seek(target)

    def _can_seek(self) -> bool:
        return (
            self._state.available
            and self._adapter is not None
            and not self._closed
            and self._state.status
            not in (PlaybackStatus.LOADING, PlaybackStatus.ERROR, PlaybackStatus.STOPPED)
        )

    def rewind(self, seconds: float = 10.0) -> float:
        amount = abs(float(seconds))
        result = self.seek(self._state.position - amount)
        self.seekFeedback.emit(f"−{int(amount)} s")
        return result

    def forward(self, seconds: float = 10.0) -> float:
        amount = abs(float(seconds))
        result = self.seek(self._state.position + amount)
        self.seekFeedback.emit(f"+{int(amount)} s")
        return result

    def _clamp_position(self, target: float) -> float:
        value = max(0.0, float(target))
        if self._state.duration > 0:
            value = min(value, self._state.duration)
        return value

    def set_volume(self, value: float) -> None:
        volume = max(0, min(100, int(value)))
        if self._adapter is not None and not self._closed:
            self._adapter.set_volume(volume)
        self._set_state(volume=volume)

    def toggle_mute(self) -> None:
        muted = not self._state.muted
        if self._adapter is not None and not self._closed:
            self._adapter.set_muted(muted)
        self._set_state(muted=muted)

    def set_muted(self, muted: bool) -> None:
        value = bool(muted)
        if self._adapter is not None and not self._closed:
            self._adapter.set_muted(value)
        self._set_state(muted=value)

    def set_speed(self, speed: float) -> None:
        value = max(0.5, min(2.0, float(speed)))
        if self._adapter is not None and not self._closed:
            self._adapter.set_speed(value)
        self._set_state(speed=value)

    def set_quality(self, height: int) -> None:
        adapter = self._adapter
        url = self._qualities.get(int(height))
        if adapter is None or self._closed or not url or url == self._current_url:
            return
        previous = self._state
        self._resume_point = _ResumePoint(
            position=previous.position,
            paused=previous.paused,
            volume=previous.volume,
            muted=previous.muted,
            speed=previous.speed,
            quality=int(height),
        )
        self._current_url = url
        self._has_loaded = False
        self._set_state(status=PlaybackStatus.LOADING, quality=int(height), error=None)
        self.qualitySwitching.emit(int(height))
        self._load_watchdog.start()
        adapter.load(url)

    def retry(self) -> None:
        adapter = self._adapter
        if adapter is None or self._closed:
            self._set_state(status=PlaybackStatus.LOADING, error=None)
            return
        self._resume_point = None
        self._has_loaded = False
        self._set_state(status=PlaybackStatus.LOADING, error=None)
        self._load_watchdog.start()
        adapter.load(self._current_url or self._initial_url)

    def fail(self, detail: str) -> None:
        if self._closed:
            return
        self._on_error(detail)

    @Slot(float)
    def _on_position(self, position: float) -> None:
        if self._resume_point is not None:
            return
        self._set_state(position=self._clamp_position(position))

    @Slot(float)
    def _on_duration(self, duration: float) -> None:
        value = max(0.0, float(duration))
        position = self._state.position
        if value > 0:
            position = min(position, value)
        self._set_state(duration=value, position=position)

    @Slot(str)
    def _on_adapter_state(self, name: str) -> None:
        status = self._STATUS_BY_ADAPTER.get(name)
        if status is None:
            return
        if not self._has_loaded and status in (
            PlaybackStatus.PLAYING,
            PlaybackStatus.PAUSED,
        ):
            return
        changes: dict[str, Any] = {
            "status": status,
            "error": None if status is not PlaybackStatus.ERROR else self._state.error,
        }
        if status in (PlaybackStatus.PAUSED, PlaybackStatus.FINISHED, PlaybackStatus.STOPPED):
            changes["paused"] = True
        elif status is PlaybackStatus.PLAYING:
            changes["paused"] = False
        self._set_state(**changes)

    @Slot(bool)
    def _on_buffering(self, buffering: bool) -> None:
        if buffering and self._state.status not in (
            PlaybackStatus.LOADING,
            PlaybackStatus.ERROR,
            PlaybackStatus.FINISHED,
        ):
            self._set_state(status=PlaybackStatus.BUFFERING)
        elif not buffering and self._state.status is PlaybackStatus.BUFFERING:
            self._set_state(
                status=PlaybackStatus.PAUSED if self._state.paused else PlaybackStatus.PLAYING
            )

    @Slot()
    def _on_media_loaded(self) -> None:
        self._load_watchdog.stop()
        self._has_loaded = True
        resume = self._resume_point
        adapter = self._adapter
        if resume is not None and adapter is not None:
            try:
                adapter.set_volume(resume.volume)
                adapter.set_muted(resume.muted)
                adapter.set_speed(resume.speed)
                adapter.set_paused(resume.paused)
                adapter.seek(self._clamp_position(resume.position))
            except Exception as exc:
                log.exception("failed to restore playback after quality switch")
                self._on_error(str(exc))
                return
            self._resume_point = None
            status = PlaybackStatus.PAUSED if resume.paused else PlaybackStatus.PLAYING
            self._set_state(
                position=self._clamp_position(resume.position),
                status=status,
                paused=resume.paused,
                error=None,
            )
        else:
            status = PlaybackStatus.PAUSED if self._state.paused else PlaybackStatus.PLAYING
            self._set_state(status=status, paused=status is PlaybackStatus.PAUSED, error=None)

    @Slot(object)
    def _on_stream_info(self, payload: Any) -> None:
        if self._closed or not isinstance(payload, dict):
            return
        self._set_state(
            stream=StreamInfo(
                width=int(payload.get("width") or 0),
                height=int(payload.get("height") or 0),
                fps=float(payload.get("fps") or 0.0),
                estimated_fps=float(payload.get("estimated_fps") or 0.0),
            )
        )

    @Slot()
    def _on_media_ended(self) -> None:
        self._set_state(status=PlaybackStatus.FINISHED, paused=True)

    @Slot(str)
    def _on_error(self, detail: str) -> None:
        self._load_watchdog.stop()
        log.error("playback unavailable: %s", detail)
        self._resume_point = None
        self._set_state(status=PlaybackStatus.ERROR, error=str(detail))

    @Slot()
    def _on_load_timeout(self) -> None:
        if self._state.status is PlaybackStatus.LOADING:
            self._on_error("The stream did not finish loading within 30 seconds.")

    @Slot(float)
    def _on_volume(self, volume: float) -> None:
        self._set_state(volume=max(0, min(100, round(volume))))

    @Slot(bool)
    def _on_muted(self, muted: bool) -> None:
        self._set_state(muted=bool(muted))

    @Slot(float)
    def _on_speed(self, speed: float) -> None:
        self._set_state(speed=float(speed))

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._load_watchdog.stop()
        self._resume_point = None
        if self._adapter is not None:
            self._adapter.close()
            self._adapter = None
