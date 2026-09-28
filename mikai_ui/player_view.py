"""Player display states and the zoom/pan transform that drives them.

Pure logic: the machine decides the canvas scale and which chrome is visible.
It never touches mpv or a widget, so every row of the state matrix is testable
without a running player.

Scale is a multiplier on mpv's own "fit to window" render, so 1.0 is the
default fit and both directions are expressed the same way.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from enum import Enum

ZOOM_MIN = 0.25
ZOOM_MAX = 5.0
ZOOM_STEP = 0.15
ZOOM_FIT = 1.0
ZOOM_MAGIFIED = 2.2
ZOOM_MINIMISED = 0.35
PAN_THRESHOLD_PX = 3

_EPSILON = 1e-6


class ViewState(Enum):
    DEFAULT_FIT = "default_fit"
    SCROLL_OUT = "scroll_out"
    ZOOM_IN_PAN = "zoom_in_pan"
    FULLSCREEN_ACTIVE = "fullscreen_active"
    FULLSCREEN_AUTO_HIDDEN = "fullscreen_auto_hidden"


class Cursor(Enum):
    DEFAULT = "default"
    ZOOM = "zoom"
    GRAB = "grab"
    GRABBING = "grabbing"
    NONE = "none"


@dataclass(frozen=True)
class Chrome:
    """Which pieces of interface a state shows, and with what pointer."""

    window_controls: bool
    gallery_navigation: bool
    playback_toolbar: bool
    metadata_footer: bool
    source_badge: bool
    cursor: Cursor
    # the magnified state lifts the toolbar off its band and over the picture
    floating_toolbar: bool


_CHROME: dict[ViewState, Chrome] = {
    ViewState.DEFAULT_FIT: Chrome(True, True, True, True, True, Cursor.DEFAULT, False),
    ViewState.SCROLL_OUT: Chrome(True, True, True, True, True, Cursor.ZOOM, False),
    ViewState.ZOOM_IN_PAN: Chrome(True, False, True, True, True, Cursor.GRAB, True),
    ViewState.FULLSCREEN_ACTIVE: Chrome(True, False, True, False, False, Cursor.DEFAULT, False),
    ViewState.FULLSCREEN_AUTO_HIDDEN: Chrome(False, False, False, False, False, Cursor.NONE, False),
}


def chrome_for(state: ViewState) -> Chrome:
    return _CHROME[state]


def pan_limit(scale: float) -> float:
    """How far the picture may be dragged before its edge reaches the frame.

    Below the fit scale the picture is smaller than the frame, so there is
    nothing to reveal and any pan would just float the video in black.
    """
    return max(0.0, scale - ZOOM_FIT) / 2.0


def clamp_pan(value: float, scale: float) -> float:
    limit = pan_limit(scale)
    return max(-limit, min(limit, value))


def clamp_scale(value: float) -> float:
    return max(ZOOM_MIN, min(ZOOM_MAX, value))


class ViewMachine:
    """The player's view state, advanced by input and published to listeners."""

    def __init__(self) -> None:
        self._scale = ZOOM_FIT
        self._pan = (0.0, 0.0)
        self._fullscreen = False
        self._chrome_hidden = False
        self._panning = False
        self._listeners: list[Callable[["ViewMachine"], None]] = []

    # --- observation ---

    @property
    def state(self) -> ViewState:
        """Exactly one state at a time.

        A magnified picture outranks the window mode: the player opens in
        fullscreen, so letting fullscreen win would make the wheel and the
        double click do nothing at all. Windowed-only chrome is gated on
        `fullscreen` by the dialog rather than by this column.
        """
        if self._scale > ZOOM_FIT + _EPSILON:
            return ViewState.ZOOM_IN_PAN
        if self._scale < ZOOM_FIT - _EPSILON:
            return ViewState.SCROLL_OUT
        if self._fullscreen:
            return (
                ViewState.FULLSCREEN_AUTO_HIDDEN
                if self._chrome_hidden
                else ViewState.FULLSCREEN_ACTIVE
            )
        return ViewState.DEFAULT_FIT

    @property
    def chrome(self) -> Chrome:
        return chrome_for(self.state)

    @property
    def scale(self) -> float:
        return self._scale

    @property
    def pan(self) -> tuple[float, float]:
        return self._pan

    @property
    def fullscreen(self) -> bool:
        return self._fullscreen

    @property
    def panning(self) -> bool:
        return self._panning

    @property
    def can_pan(self) -> bool:
        return self.state is ViewState.ZOOM_IN_PAN

    def subscribe(self, listener: Callable[["ViewMachine"], None]) -> None:
        self._listeners.append(listener)

    def _publish(self) -> None:
        for listener in self._listeners:
            listener(self)

    # --- canvas ---

    def _apply_scale(self, scale: float) -> None:
        self._scale = clamp_scale(scale)
        self._pan = (
            clamp_pan(self._pan[0], self._scale),
            clamp_pan(self._pan[1], self._scale),
        )
        self._publish()

    def zoom_by(self, delta: float) -> None:
        self._apply_scale(self._scale + delta)

    def set_scale(self, scale: float) -> None:
        self._apply_scale(scale)

    def toggle_magnify(self) -> None:
        """Double click: jump to the magnified state, or back to the fit."""
        self._apply_scale(ZOOM_FIT if self._scale > ZOOM_FIT + _EPSILON else ZOOM_MAGIFIED)

    def pan_by(self, dx: float, dy: float) -> None:
        if not self.can_pan:
            return
        x, y = self._pan
        self._pan = (clamp_pan(x + dx, self._scale), clamp_pan(y + dy, self._scale))
        self._publish()

    def set_panning(self, panning: bool) -> None:
        self._panning = panning

    def reset_canvas(self) -> None:
        self._pan = (0.0, 0.0)
        self._apply_scale(ZOOM_FIT)

    # --- window mode ---

    def enter_fullscreen(self) -> None:
        self._fullscreen = True
        self._chrome_hidden = False
        self._publish()

    def exit_fullscreen(self) -> None:
        self._fullscreen = False
        self._chrome_hidden = False
        self._publish()

    def hide_chrome(self) -> None:
        """The fullscreen idle timer fired."""
        if not self._fullscreen or self._chrome_hidden:
            return
        self._chrome_hidden = True
        self._publish()

    def wake(self) -> None:
        """Pointer or key activity, from either fullscreen state."""
        if not self._fullscreen or not self._chrome_hidden:
            return
        self._chrome_hidden = False
        self._publish()
