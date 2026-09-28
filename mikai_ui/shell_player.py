"""The mikai player: the vendored PlayerShell hosted over mikai's mpv engine.

The shell paints its own chrome and owns no playback logic, so this module is
the bridge in both directions plus the three panels the shell has no room for:
the quality list, the episode drawer and the dub/sub picker. They float over the
shell as ordinary Qt children, because a shell that paints its own dock cannot
accept injected buttons.
"""

from __future__ import annotations

import logging
from pathlib import Path

import shiboken6
from PySide6.QtCore import QEvent, QObject, QPoint, Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QDesktopServices, QGuiApplication
from PySide6.QtWidgets import (
    QApplication,
    QDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from mikai_ui.media_shell import (
    ASPECT_FALLBACK,
    STATE_FULLSCREEN_ACTIVE,
    STATE_FULLSCREEN_AUTO_HIDDEN,
    PlayerShell,
    fmt_time,
)
from mikai_ui.mpv_backend import MPVAdapter
from mikai_ui.pip_window import PipWindow
from mikai_ui.player_controller import PlaybackState, PlaybackStatus, PlayerController
from mikai_ui.theme.colors import BORDER, ICON_DISABLED, MUTED, TEXT
from mikai_ui.theme.metrics import (
    ICON_SIZE,
    PIP_DEFAULT_HEIGHT,
    PIP_DEFAULT_WIDTH,
    PLAYER_DEFAULT_HEIGHT,
    PLAYER_DEFAULT_WIDTH,
    PLAYER_MIN_HEIGHT,
    PLAYER_MIN_WIDTH,
    SPACING_MEDIUM,
    SPACING_SMALL,
    SPACING_XS,
)
from mikai_ui.theme.widgets import make_icon

log = logging.getLogger(__name__)

# The shell's speed control tops out at 2.0x. The engine has no such limit, so
# only the value handed to the shell is clamped; mpv gets what was asked for.
SHELL_RATE_MIN = 0.25
SHELL_RATE_MAX = 2.0

SERIES_WIDTH = 190
ROW_HEIGHT = 28
STRIP_HEIGHT = 30
STATUS_MS = 1900
STATUS_LABEL_WIDTH = 560
STRIP_MARGIN = 8

# Qt's QWIDGETSIZE_MAX is a C++ macro that PySide6 does not export
UNLIMITED = 16777215

# The vendored shell names none of its widgets, so the gear that opens the
# settings flyout is found by the only tooltip it carries.
SETTINGS_TOOLTIP = "toggle_settings_popover"
FULLSCREEN_STATES = frozenset({STATE_FULLSCREEN_ACTIVE, STATE_FULLSCREEN_AUTO_HIDDEN})
PLAYING_STATES = frozenset({PlaybackStatus.PLAYING, PlaybackStatus.BUFFERING})

PANEL_QSS = (
    'QFrame[chromePanel="true"] {'
    "  background-color: rgba(11, 11, 15, 0.92);"
    f"  border: 1px solid {BORDER};"
    "  border-radius: 10px; }"
)


def _row_qss(foreground: str, fill: str) -> str:
    return (
        "QPushButton {"
        f"  color: {foreground};"
        f"  background: {fill};"
        "  border: none; border-radius: 6px; text-align: left;"
        f"  padding: 0 {SPACING_SMALL}px; min-height: 24px; }}"
        "QPushButton:hover { background: rgba(231, 231, 234, 0.12); }"
    )


def _chip_qss() -> str:
    return (
        "QPushButton {"
        f"  color: {TEXT}; background: rgba(11, 11, 15, 0.72);"
        f"  border: 1px solid {BORDER}; border-radius: 8px;"
        f"  padding: 0 {SPACING_SMALL}px; font-size: 12px; }}"
        "QPushButton:hover { background: rgba(231, 231, 234, 0.12); }"
    )


def _caption_qss(size: int = 11) -> str:
    return f"color: {MUTED}; font-size: {size}px; background: transparent;"


class _MenuPopup(QFrame):
    """A frameless popover in the shell's own flyout idiom.

    Its rows are NoFocus on purpose: a focused QPushButton swallows Space, and
    Space belongs to the shell as play/pause. The popover takes focus itself and
    drives the rows with the arrow keys, so it stays keyboard reachable without
    taking a key the player cannot spare.
    """

    picked = Signal(object)

    def __init__(self, title: str, parent: QWidget | None = None) -> None:
        super().__init__(parent, Qt.WindowType.Popup | Qt.WindowType.FramelessWindowHint)
        self.setProperty("chromePanel", True)
        self.setObjectName("playerMenuPopup")
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setStyleSheet(PANEL_QSS)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(SPACING_SMALL, SPACING_SMALL, SPACING_SMALL, SPACING_XS)
        outer.setSpacing(SPACING_XS)

        caption = QLabel(title, self)
        caption.setObjectName("playerMenuCaption")
        caption.setStyleSheet(_caption_qss())
        outer.addWidget(caption)

        self._body = QVBoxLayout()
        self._body.setContentsMargins(0, 0, 0, 0)
        self._body.setSpacing(2)
        outer.addLayout(self._body)

        # a long hint would set the popover's width, and a quality list two
        # rows high does not want to be 500 px wide
        hint = QLabel("↑↓ · Enter · Esc", self)
        hint.setObjectName("playerMenuHint")
        hint.setStyleSheet(_caption_qss(size=10))
        outer.addWidget(hint)
        self.setToolTip("Керування: ↑ ↓ — вибір, Enter — підтвердити, Esc — закрити")

        self._rows: list[QPushButton] = []
        self._row_entries: list[dict] = []
        self._highlight = -1

    @property
    def row_count(self) -> int:
        return len(self._rows)

    def set_entries(self, entries: list[dict]) -> None:
        """Rebuild from scratch: a swapped stream replaces the whole packet
        behind a row, so repainting the standing rows would show the qualities
        of whatever was playing before."""
        while self._body.count():
            widget = self._body.takeAt(0).widget()
            if widget is not None:
                widget.setParent(None)
                widget.deleteLater()
        self._rows = []
        self._row_entries = []
        self._highlight = -1
        for entry in entries:
            heading = entry.get("heading")
            if heading:
                label = QLabel(str(heading), self)
                label.setObjectName("playerMenuHeading")
                label.setStyleSheet(f"{_caption_qss()} padding: 4px 6px;")
                self._body.addWidget(label)
                continue
            text = str(entry.get("text", ""))
            button = QPushButton(text, self)
            button.setObjectName("playerMenuRow")
            button.setToolTip(str(entry.get("tooltip", "")) or text)
            button.setAccessibleName(text)
            button.setCursor(Qt.CursorShape.PointingHandCursor)
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            button.setFixedHeight(ROW_HEIGHT)
            spot = len(self._rows)
            button.clicked.connect(lambda _checked=False, index=spot: self._activate(index))
            self._body.addWidget(button)
            self._rows.append(button)
            self._row_entries.append(entry)
        self._paint_rows()
        self._fit_to_hint()

    def refresh(self) -> None:
        self._fit_to_hint()

    def popup_near(self, anchor: QPoint, *, prefer_above: bool = True) -> None:
        self._fit_to_hint()
        screen = QGuiApplication.screenAt(anchor) or QGuiApplication.primaryScreen()
        above = anchor.y() - self.height() - 8
        x = anchor.x() - self.width()
        y = above if prefer_above else anchor.y() + 8
        if screen is not None:
            bounds = screen.availableGeometry()
            if prefer_above and above < bounds.top() + 4:
                y = anchor.y() + 8
            x = min(max(x, bounds.left() + 4), bounds.right() - self.width() - 4)
            y = min(max(y, bounds.top() + 4), bounds.bottom() - self.height() - 4)
        self.move(x, y)
        self.show()
        self.raise_()
        self.setFocus(Qt.FocusReason.OtherFocusReason)

    def keyPressEvent(self, event) -> None:
        key = event.key()
        if key == Qt.Key.Key_Escape:
            self.close()
        elif key in (Qt.Key.Key_Down, Qt.Key.Key_Right):
            self._move(1)
        elif key in (Qt.Key.Key_Up, Qt.Key.Key_Left):
            self._move(-1)
        elif key == Qt.Key.Key_Home:
            self._set_highlight(0)
        elif key == Qt.Key.Key_End:
            self._set_highlight(len(self._rows) - 1)
        elif key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            if self._highlight >= 0:
                self._activate(self._highlight)
        else:
            super().keyPressEvent(event)
            return
        event.accept()

    def _fit_to_hint(self) -> None:
        # a previous popup_near pinned both edges, and adjustSize alone would be
        # bounded by that stale geometry
        self.setMinimumSize(0, 0)
        self.setMaximumSize(UNLIMITED, UNLIMITED)
        self.setFixedSize(self.sizeHint())

    def _move(self, delta: int) -> None:
        if not self._rows:
            return
        if self._highlight < 0:
            self._set_highlight(0 if delta > 0 else len(self._rows) - 1)
            return
        self._set_highlight((self._highlight + delta) % len(self._rows))

    def _set_highlight(self, spot: int) -> None:
        if not self._rows:
            return
        self._highlight = min(max(spot, 0), len(self._rows) - 1)
        self._paint_rows()

    def _activate(self, spot: int) -> None:
        if not 0 <= spot < len(self._row_entries):
            return
        entry = self._row_entries[spot]
        self.close()
        self.picked.emit(entry.get("payload"))

    def _paint_rows(self) -> None:
        for index, row in enumerate(self._rows):
            if index < len(self._row_entries) and self._row_entries[index].get("current"):
                row.setStyleSheet(_row_qss(TEXT, "rgba(189, 172, 255, 0.20)"))
            elif index == self._highlight:
                row.setStyleSheet(_row_qss(TEXT, "rgba(231, 231, 234, 0.10)"))
            else:
                row.setStyleSheet(_row_qss(MUTED, "transparent"))


class _QualityPopup(_MenuPopup):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__("ЯКІСТЬ", parent)
        self.setObjectName("playerQualityPopup")


class _TrackPopup(_MenuPopup):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__("ОЗВУЧЕННЯ ТА СУБТИТРИ", parent)
        self.setObjectName("playerTrackPopup")


class _SeriesDrawer(QFrame):
    """The episode list, opened from the strip and closed by the same button."""

    picked = Signal(int)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("playerSeriesDrawer")
        self.setProperty("chromePanel", True)
        self.setStyleSheet(PANEL_QSS)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(SPACING_XS, SPACING_XS, SPACING_XS, SPACING_XS)
        outer.setSpacing(SPACING_XS)

        caption = QLabel("СЕРІЇ", self)
        caption.setObjectName("playerSeriesCaption")
        caption.setStyleSheet(_caption_qss())
        outer.addWidget(caption)

        self._scroll = QScrollArea(self)
        self._scroll.setObjectName("playerSeriesScroll")
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QFrame.Shape.NoFrame)
        self._scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._content = QWidget()
        self._content.setObjectName("playerSeriesList")
        self._rows_layout = QVBoxLayout(self._content)
        self._rows_layout.setContentsMargins(0, 0, 0, 0)
        self._rows_layout.setSpacing(2)
        self._scroll.setWidget(self._content)
        outer.addWidget(self._scroll, 1)

        self._rows: list[QPushButton] = []

    def set_entries(self, labels: list[str], current: int) -> None:
        while self._rows_layout.count():
            widget = self._rows_layout.takeAt(0).widget()
            if widget is not None:
                widget.setParent(None)
                widget.deleteLater()
        self._rows = []
        for spot, label in enumerate(labels):
            button = QPushButton(str(label), self._content)
            button.setObjectName(
                "playerSeriesItemCurrent" if spot == current else "playerSeriesItem"
            )
            button.setCursor(Qt.CursorShape.PointingHandCursor)
            button.setToolTip(str(label))
            button.setAccessibleName(str(label))
            # a focused button would swallow Space, and Space is play/pause
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            button.setFixedHeight(ROW_HEIGHT)
            button.clicked.connect(lambda _checked=False, index=spot: self.picked.emit(index))
            self._rows_layout.addWidget(button)
            self._rows.append(button)
            button.setStyleSheet(
                _row_qss(TEXT, "rgba(189, 172, 255, 0.20)")
                if spot == current
                else _row_qss(MUTED, "transparent")
            )
        self._rows_layout.addStretch(1)

    def content_height(self) -> int:
        return len(self._rows) * (ROW_HEIGHT + 2) + 3 * SPACING_XS + 20


class ShellPlayerDialog(QDialog):
    """Own the widget composition and connect the shell's intent to the engine."""

    runtimeRequested = Signal()
    playbackFailed = Signal()
    variantRequested = Signal(str, str)
    episodeRequested = Signal(object, int)
    trackProbeRequested = Signal(str)

    def __init__(
        self,
        caption: str,
        url: str,
        qualities: dict[int, str] | None = None,
        initial_height: int | None = None,
        variants: list[dict] | None = None,
        current_embed: str = "",
        playlist: list[dict] | None = None,
        episode_index: int = 0,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._caption = caption
        self._url = url
        self._qualities = self._usable_qualities(qualities)
        self._variants = [dict(variant) for variant in (variants or [])]
        self._current_embed = current_embed
        self._current_height = initial_height
        self._playlist: list[dict] = []
        self._episode_index = 0
        # the title page writes the playing episode here, so a stream recovery
        # re-extracts the track that is playing, not the one we opened with
        self._page_entry: dict | None = None

        self._runtime_dir: Path | None = None
        self._closed = False
        self._refresh_attempted = False
        self._aspect = ASPECT_FALLBACK
        self._fullscreen = False
        self._series_expanded = False
        self._error_active = False
        self._pip: PipWindow | None = None
        self._pip_was_fullscreen = False

        self.setObjectName("playerWindow")
        self.setWindowTitle(f"Плеєр — {caption}")
        self.setWindowFlag(Qt.WindowType.FramelessWindowHint, True)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        self.setMinimumSize(PLAYER_MIN_WIDTH, PLAYER_MIN_HEIGHT)
        self.resize(PLAYER_DEFAULT_WIDTH, PLAYER_DEFAULT_HEIGHT)
        self.setContentsMargins(0, 0, 0, 0)

        self._shell = PlayerShell(self, frameless=True, demo_clock=False)
        self._shell.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        root.addWidget(self._shell)
        # every key belongs to the shell, the one widget that acts on them: the
        # panels are all NoFocus, so this is what routes Space to play/pause
        self.setFocusProxy(self._shell)

        self._build_strip()
        self._build_status()
        self._series = _SeriesDrawer(self)
        self._quality_popup = _QualityPopup(self)
        self._track_popup = _TrackPopup(self)
        for panel in (self._strip, self._status, self._series):
            # mpv paints into a native child of the shell, so a panel floating
            # over the picture needs its own native handle to stay above it.
            # WA_DontCreateNativeAncestors is what keeps that handle to the
            # panel: without it Qt promotes every ancestor, and the shell and
            # the dialog go native too, which breaks compositing between the
            # shell's chrome and the feature strip sitting above it.
            panel.setAttribute(Qt.WidgetAttribute.WA_DontCreateNativeAncestors, True)
            panel.setAttribute(Qt.WidgetAttribute.WA_NativeWindow, True)

        self._quality_popup.picked.connect(self._on_quality_picked)
        self._track_popup.picked.connect(self._on_track_picked)
        self._series.picked.connect(lambda spot: self._go_to_episode(spot, close_list=True))

        self._controller = PlayerController(url, self._qualities, initial_height, self)
        self._controller.stateChanged.connect(self._on_state_changed)
        self._controller.qualitySwitching.connect(self._on_quality_switching)
        self._connect_shell()

        self.set_playlist(playlist, episode_index)
        self._track_button.setVisible(len(self._variants) > 1)
        self._sync_quality_button()
        self._shell.set_media(
            url, origin=self._caption, quality=f"{initial_height}p" if initial_height else ""
        )
        self._on_state_changed(self._controller.state)

    # ── construction ───────────────────────────────────────────────────

    def _build_strip(self) -> None:
        self._strip = QWidget(self)
        self._strip.setObjectName("playerFeatureStrip")
        self._strip.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        row = QHBoxLayout(self._strip)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(SPACING_XS)

        self._series_button = QPushButton(self._strip)
        self._series_button.setObjectName("playerSeriesToggle")
        self._series_button.setFixedSize(STRIP_HEIGHT, STRIP_HEIGHT)
        self._series_button.setCursor(Qt.CursorShape.PointingHandCursor)
        self._series_button.setToolTip("Список серій (S)")
        self._series_button.setAccessibleName("Список серій")
        self._series_button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._series_button.setStyleSheet(_chip_qss())
        self._series_button.clicked.connect(self.toggle_series)
        self._update_series_toggle()
        row.addWidget(self._series_button)

        self._track_button = self._make_chip(self._strip, "playerTrackButton", "Доріжки")
        self._track_button.setToolTip("Озвучення та субтитри (T)")
        self._track_button.setAccessibleName("Озвучення та субтитри")
        self._track_button.clicked.connect(self.open_track_menu)
        row.addWidget(self._track_button)

        self._quality_button = self._make_chip(self._strip, "playerQualityButton", "Якість")
        self._quality_button.setToolTip("Якість відео (Q)")
        self._quality_button.setAccessibleName("Якість відео")
        self._quality_button.clicked.connect(self.open_quality_menu)
        row.addWidget(self._quality_button)

    def _make_chip(self, parent: QWidget, name: str, text: str) -> QPushButton:
        button = QPushButton(text, parent)
        button.setObjectName(name)
        button.setFixedHeight(STRIP_HEIGHT)
        button.setCursor(Qt.CursorShape.PointingHandCursor)
        # a focused button would swallow Space, and Space is play/pause
        button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        button.setStyleSheet(_chip_qss())
        return button

    def _build_status(self) -> None:
        self._status = QFrame(self)
        self._status.setObjectName("playerStatusPanel")
        self._status.setProperty("chromePanel", True)
        self._status.setStyleSheet(PANEL_QSS)
        row = QHBoxLayout(self._status)
        row.setContentsMargins(SPACING_SMALL, SPACING_XS, SPACING_XS, SPACING_XS)
        row.setSpacing(SPACING_SMALL)

        self._status_label = QLabel("", self._status)
        self._status_label.setObjectName("playerStatusText")
        self._status_label.setStyleSheet(f"color: {TEXT}; background: transparent;")
        self._status_label.setMaximumWidth(STATUS_LABEL_WIDTH)
        row.addWidget(self._status_label, 1)

        self._retry_button = self._make_chip(self._status, "playerRetryButton", "Повторити")
        self._retry_button.setToolTip("Повторити відтворення")
        self._retry_button.setAccessibleName("Повторити відтворення")
        self._retry_button.clicked.connect(self._retry_playback)
        row.addWidget(self._retry_button, 0)
        self._status.hide()

        self._status_timer = QTimer(self)
        self._status_timer.setSingleShot(True)
        self._status_timer.timeout.connect(self._status.hide)

    def _connect_shell(self) -> None:
        shell = self._shell
        shell.playPauseRequested.connect(self._controller.toggle_play_pause)
        shell.seekRequested.connect(self._controller.seek)
        shell.rateChanged.connect(self._controller.set_speed)
        shell.volumeChanged.connect(self._on_volume)
        shell.muteChanged.connect(self._controller.set_muted)
        shell.prevRequested.connect(lambda: self.step_episode(-1))
        shell.nextRequested.connect(lambda: self.step_episode(1))
        shell.pipRequested.connect(self.toggle_pip)
        shell.openOriginalRequested.connect(self._open_stream_page)
        shell.downloadRequested.connect(self._copy_stream_url)
        shell.mediaDropped.connect(self._on_media_dropped)
        shell.closeRequested.connect(self.close)
        shell.stateChanged.connect(self._on_shell_state)
        shell.fullscreenChanged.connect(self._on_shell_fullscreen)
        shell.installEventFilter(self)

    @staticmethod
    def _usable_qualities(qualities: dict[int, str] | None) -> dict[int, str]:
        return {
            int(height): url
            for height, url in (qualities or {}).items()
            if isinstance(url, str) and url.strip()
        }

    @property
    def controller(self) -> PlayerController:
        return self._controller

    # ── engine <-> shell ───────────────────────────────────────────────

    def _on_volume(self, value: float) -> None:
        """The shell speaks 0…1; the engine and mpv speak 0…100."""
        self._controller.set_volume(round(max(0.0, min(1.0, float(value))) * 100))

    def _on_state_changed(self, state: PlaybackState) -> None:
        if self._closed or not shiboken6.isValid(self._shell):
            return
        self._sync_pip_bar(state)
        if state.status is PlaybackStatus.ERROR:
            self._show_error(state.error or "Не вдалося відтворити потік")
            if not self._refresh_attempted:
                self._refresh_attempted = True
                self.playbackFailed.emit()
        elif self._error_active:
            self._error_active = False
            self._status_timer.stop()
            self._status.hide()
        if not state.available:
            return
        # the shell's timeline is a scrub bar over a known length: a position
        # fed before mpv reports one only paints 0:00 / -0:00
        if state.duration > 0:
            self._shell.set_duration(state.duration)
            self._shell.set_position(state.position)
        self._shell.set_playing(state.status in PLAYING_STATES)
        # the shell drops its own mute when a volume arrives, so the mute state
        # is pushed after it or the two would fight
        self._shell.set_volume(state.volume / 100.0)
        self._shell.set_muted(state.muted)
        self._shell.set_rate(min(SHELL_RATE_MAX, max(SHELL_RATE_MIN, state.speed)))
        stream = state.stream
        if stream is not None and stream.width > 0 and stream.height > 0:
            self._aspect = stream.width / stream.height
            self._shell.set_aspect_ratio(self._aspect)
        if state.quality:
            self._current_height = state.quality
            self._shell.set_quality(f"{state.quality}p")
        self._sync_quality_button()

    def _on_shell_state(self, state: str) -> None:
        if self._closed:
            return
        # the state drives CHROME only. It used to drive the window as well, so
        # zooming — which is not a full-screen state — dropped the player into
        # a movable window in the middle of playback.
        fullscreen = state in FULLSCREEN_STATES
        if fullscreen != self._fullscreen:
            self._fullscreen = fullscreen
        if fullscreen:
            self._quality_popup.close()
            self._track_popup.close()
        # The strip and the drawer follow their own state, not the window's.
        # They used to hide whenever the picture was full screen, which was a
        # rule for a player that could also be a window: now that full screen
        # is the only mode, that rule hid the three features permanently.
        self._series.setVisible(self._series_expanded)
        self._strip.setVisible(True)
        self._layout_panels()

    def _on_shell_fullscreen(self, fullscreen: bool) -> None:
        """The player has no windowed mode; the float is the movable window.

        Anything that would drop the player out of full screen — F, F11, Esc,
        the dock's fullscreen control — lands here and is undone.
        """
        if self._closed:
            return
        if not fullscreen:
            self._shell._enter_fullscreen()
        self._sync_window_fullscreen()

    def _sync_window_fullscreen(self) -> None:
        """The shell is a child widget, so its own showFullScreen() is a no-op:
        the window itself is what has to go full screen."""
        if self.pip_active:
            return
        self.showFullScreen()

    def _on_quality_switching(self, height: int) -> None:
        self._flash_status(f"Перемикання на {height}p…")

    def _on_media_dropped(self, path: str) -> None:
        if self._closed or not path:
            return
        # the dropped file replaces the episode, so the qualities of the stream
        # just left behind would offer to switch back into what is not playing
        self._qualities = {}
        self._current_height = None
        self._sync_quality_button()
        self._shell.set_media(path, origin="Локальний файл", quality="")
        self._controller.replace_stream(path, {})
        self._flash_status("Відтворюю локальний файл")

    # ── episodes ───────────────────────────────────────────────────────

    def set_playlist(self, playlist: list[dict] | None, index: int = 0) -> None:
        self._playlist = [dict(item) for item in (playlist or [])]
        self._episode_index = index if 0 <= index < len(self._playlist) else 0
        self._series_button.setVisible(bool(self._playlist))
        self._series.set_entries(
            [str(item.get("label", "")) for item in self._playlist], self._episode_index
        )
        self._series.setVisible(self._series_expanded)
        self._layout_panels()

    def step_episode(self, delta: int) -> None:
        self._go_to_episode(self._episode_index + delta)

    def _go_to_episode(self, position: int, *, close_list: bool = False) -> None:
        """Step or jump to an episode.

        Picking from the list closes it, because that means "this one, now"; the
        flank chevrons leave it open, since stepping while browsing a long list
        and having it snap shut under the pointer is a fight.
        """
        if self._closed or not 0 <= position < len(self._playlist):
            return
        if close_list:
            self.set_series_expanded(False)
        self.episodeRequested.emit(dict(self._playlist[position]), position)

    def apply_episode(
        self,
        label: str,
        embed: str,
        index: int,
        qualities: dict[int, str],
        variants: list[dict],
    ) -> None:
        """Step to another episode in place: same window, same position.

        The swap goes through the controller's variant machine, so playback
        continues where it was instead of restarting, and no second player is
        ever stacked.
        """
        if self._closed or not qualities:
            return
        self._episode_index = index
        self._variants = [dict(variant) for variant in variants]
        self._current_embed = embed
        self._track_button.setVisible(len(self._variants) > 1)
        self._set_qualities(qualities)
        target = max(self._qualities)
        self._caption = f"{label} · {target}p"
        self.setWindowTitle(f"Плеєр — {self._caption}")
        self.set_playlist(self._playlist, index)
        self._flash_status(f"Серія: {label}")
        self._controller.switch_variant(self._qualities[target], self._qualities, target)
        self._shell.set_media(
            self._controller.current_url, origin=self._caption, quality=f"{target}p"
        )

    def apply_variant(self, qualities: dict[int, str], label: str, embed: str = "") -> None:
        """Swap the dub/sub track; the controller resumes at the same position."""
        if self._closed or not qualities:
            return
        if embed:
            self._current_embed = embed
        self._set_qualities(qualities)
        target = max(self._qualities)
        self._flash_status(f"Доріжка: {label}")
        self._controller.switch_variant(self._qualities[target], self._qualities, target)
        self._shell.set_media(
            self._controller.current_url, origin=self._caption, quality=f"{target}p"
        )

    def apply_refreshed_streams(
        self,
        qualities: dict[int, str],
        height: int | None = None,
    ) -> None:
        """Reload from a re-extracted quality map after a failed stream load."""
        if self._closed or not qualities:
            return
        self._set_qualities(qualities)
        target = height if height in self._qualities else max(self._qualities)
        self._flash_status("Посилання на потік прострочене — оновлюю")
        self._controller.replace_stream(self._qualities[target], self._qualities)
        self._shell.set_media(
            self._controller.current_url, origin=self._caption, quality=f"{target}p"
        )

    def _set_qualities(self, qualities: dict[int, str]) -> None:
        self._qualities = self._usable_qualities(qualities)
        self._sync_quality_button()
        if self._quality_popup.isVisible():
            self._fill_quality_menu()

    # ── tracks ─────────────────────────────────────────────────────────

    @staticmethod
    def _track_row_text(variant: dict) -> str:
        """`<release> · <provider>` plus the badge once a probe found it."""
        display = variant.get("display")
        track = variant.get("track")
        base = f"{display} · {track}" if display and track else str(variant.get("label", ""))
        quality = int(variant.get("quality") or 0)
        return f"{base} · {quality}p" if quality > 0 else base

    def _track_entries(self) -> list[dict]:
        entries: list[dict] = []
        heading = ""
        for variant in self._variants:
            embed = str(variant.get("embed") or "")
            if not embed:
                continue
            kind = str(variant.get("kind_label") or "")
            if kind != heading:
                heading = kind
                if kind:
                    entries.append({"heading": kind})
            text = self._track_row_text(variant)
            entries.append(
                {
                    "text": text,
                    "tooltip": str(variant.get("label") or text),
                    "current": embed == self._current_embed,
                    "payload": {"embed": embed, "label": str(variant.get("label") or text)},
                }
            )
        return entries

    def _probe_unknown_tracks(self) -> None:
        """Ask for the qualities the popover could not show yet.

        Lazy on purpose: a track nobody looks at never costs a request.
        """
        if self._closed:
            return
        for variant in self._variants:
            embed = str(variant.get("embed") or "")
            if not embed or embed == self._current_embed:
                continue
            if int(variant.get("quality") or 0) <= 0:
                self.trackProbeRequested.emit(embed)

    def set_track_quality(self, embed: str, quality: int) -> None:
        """Badge one track with the quality a probe found for it."""
        if self._closed or quality <= 0:
            return
        for variant in self._variants:
            if variant.get("embed") == embed:
                variant["quality"] = quality
        if self._track_popup.isVisible():
            self._track_popup.set_entries(self._track_entries())

    def _on_track_picked(self, payload: object) -> None:
        if self._closed or not isinstance(payload, dict):
            return
        embed = str(payload.get("embed") or "")
        label = str(payload.get("label") or embed)
        if embed and embed != self._current_embed:
            self.variantRequested.emit(embed, label)

    # ── the three panels ───────────────────────────────────────────────

    def _fill_quality_menu(self) -> None:
        entries = [
            {
                "text": f"{height}p",
                "tooltip": f"Якість {height}p",
                "current": height == self._controller.state.quality,
                "payload": height,
            }
            for height in sorted(self._qualities, reverse=True)
        ]
        self._quality_popup.set_entries(entries)

    def open_quality_menu(self) -> None:
        if self._closed:
            return
        if not self._qualities:
            self._flash_status("Якість недоступна")
            return
        self._fill_quality_menu()
        if self._quality_popup.row_count == 0:
            return
        self._quality_popup.popup_near(self._settings_anchor())

    def _on_quality_picked(self, height: object) -> None:
        if self._closed or not isinstance(height, int):
            return
        self._controller.set_quality(height)

    def open_track_menu(self) -> None:
        if self._closed:
            return
        entries = self._track_entries()
        if not entries:
            self._flash_status("Немає інших доріжок для цієї серії")
            return
        self._track_popup.set_entries(entries)
        if self._track_popup.row_count == 0:
            return
        self._probe_unknown_tracks()
        anchor = self._track_button.mapToGlobal(QPoint(0, self._track_button.height()))
        self._track_popup.popup_near(anchor, prefer_above=False)

    def _settings_anchor(self) -> QPoint:
        for button in self._shell.findChildren(QPushButton):
            if button.toolTip() == SETTINGS_TOOLTIP:
                return button.mapToGlobal(QPoint(button.width(), 0))
        return self._shell.mapToGlobal(QPoint(self._shell.width() // 2, self._shell.height() - 46))

    def _sync_quality_button(self) -> None:
        height = self._controller.state.quality or self._current_height
        self._quality_button.setText(f"Якість {height}p" if height else "Якість")
        self._quality_button.setVisible(bool(self._qualities))

    def toggle_series(self) -> None:
        self.set_series_expanded(not self._series_expanded)

    def set_series_expanded(self, expanded: bool) -> None:
        self._series_expanded = bool(expanded)
        if not self._series_expanded:
            self._quality_popup.close()
            self._track_popup.close()
        self._update_series_toggle()
        self._series.setVisible(self._series_expanded)
        self._layout_panels()

    def _update_series_toggle(self) -> None:
        hint = "Сховати список серій (S)" if self._series_expanded else "Список серій (S)"
        self._series_button.setToolTip(hint)
        self._series_button.setAccessibleName(hint)
        self._series_button.setIcon(
            make_icon(
                "chevron_left" if self._series_expanded else "chevron_right",
                size=ICON_SIZE,
                disabled_color=ICON_DISABLED,
            )
        )

    # ── status ─────────────────────────────────────────────────────────

    def _flash_status(self, message: str) -> None:
        if self._closed or not shiboken6.isValid(self._status):
            return
        self._error_active = False
        self._retry_button.hide()
        self._status_label.setText(str(message))
        self._status_timer.start(STATUS_MS)
        self._status.show()
        self._status.raise_()

    def _show_error(self, message: str) -> None:
        if self._closed or not shiboken6.isValid(self._status):
            return
        self._error_active = True
        self._status_timer.stop()
        self._status_label.setText(str(message))
        self._retry_button.show()
        self._status.show()
        self._status.raise_()

    def _retry_playback(self) -> None:
        if self._closed:
            return
        if self._controller.adapter is None:
            self.runtimeRequested.emit()
        else:
            self._controller.retry()

    # ── lifecycle ──────────────────────────────────────────────────────

    def start_player(self, runtime_dir: Path | None = None) -> None:
        if self._closed or self._controller.adapter is not None:
            return
        self._runtime_dir = Path(runtime_dir) if runtime_dir is not None else None
        try:
            adapter = MPVAdapter(self._shell.video_surface(), mpv_path=self._runtime_dir)
            self._controller.attach_adapter(adapter)
            self._controller.load()
        except Exception as exc:
            log.exception("player startup failed")
            self._controller.fail(str(exc))

    def show_error(self, message: str) -> None:
        self._controller.fail(message)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._layout_panels()

    def keyPressEvent(self, event) -> None:
        """Every key goes to the shell, the only widget that acts on them; a
        dialog that swallowed one would take play/pause away with it."""
        if self._closed or not shiboken6.isValid(self._shell):
            super().keyPressEvent(event)
            return
        QApplication.sendEvent(self._shell, event)

    def showEvent(self, event) -> None:
        super().showEvent(event)
        # PlayerShell hides panels while building itself, which sets the
        # explicit-hide flag; a layout child in that state never appears on its
        # own, so the whole player would render as an empty frame.
        self._shell.show()
        self._shell.setFocus(Qt.FocusReason.OtherFocusReason)
        self._layout_panels()
        # opened full screen and stays that way
        self._shell._enter_fullscreen()

    def _layout_panels(self) -> None:
        area = self._shell.geometry()
        self._strip.adjustSize()
        self._strip.move(area.x() + SPACING_MEDIUM, area.y() + STRIP_MARGIN)
        hint = self._status.sizeHint()
        self._status.setGeometry(
            area.x() + (area.width() - hint.width()) // 2,
            area.y() + STRIP_MARGIN,
            hint.width(),
            hint.height(),
        )
        if self._series_expanded:
            top = area.y() + STRIP_MARGIN + STRIP_HEIGHT + SPACING_XS
            height = min(self._series.content_height(), area.height() - top - SPACING_MEDIUM)
            self._series.setGeometry(area.x() + SPACING_MEDIUM, top, SERIES_WIDTH, max(height, 60))
        for panel in (self._strip, self._status, self._series):
            if panel.isVisible():
                panel.raise_()

    # ── picture in picture ─────────────────────────────────────────────

    @property
    def pip_active(self) -> bool:
        return self._pip is not None and shiboken6.isValid(self._pip)

    def toggle_pip(self) -> None:
        """Float the video in a borderless always-on-top window.

        The SAME surface widget is reparented rather than a second mpv instance:
        one decoder and one position means the float can be opened and closed
        mid-playback without re-seeking, buffering or drifting.
        """
        if self._closed or not shiboken6.isValid(self._shell):
            return
        if self.pip_active:
            self._leave_pip()
        else:
            self._enter_pip()

    def _enter_pip(self) -> None:
        self._pip_was_fullscreen = self.isFullScreen()
        if self._pip_was_fullscreen:
            self.showNormal()
        # parentless on purpose: a child window is clipped to its parent's
        # z-order, so a float parented to the player slides under whatever app
        # the user switches to
        window = PipWindow()
        window.togglePlayRequested.connect(self._controller.toggle_play_pause)
        window.seekRequested.connect(
            lambda delta: self._controller.seek(self._controller.state.position + delta)
        )
        window.closeRequested.connect(self._leave_pip)
        window.set_video(self._shell.video_surface())
        window.resize(PIP_DEFAULT_WIDTH, PIP_DEFAULT_HEIGHT)
        screen = self.screen() or QGuiApplication.primaryScreen()
        if screen is not None:
            area = screen.availableGeometry()
            window.move(
                area.right() - PIP_DEFAULT_WIDTH - 24, area.bottom() - PIP_DEFAULT_HEIGHT - 24
            )
        window.installEventFilter(self)
        self._pip = window
        window.show()
        window.raise_()
        self._sync_pip_bar(self._controller.state)
        self._flash_status("Картинка в картинці")
        # the float owns the picture now, so what is left behind would be an
        # empty black frame
        self.hide()

    def _leave_pip(self, *, closing: bool = False) -> None:
        window, self._pip = self._pip, None
        if window is not None and shiboken6.isValid(window):
            video = window.release_video()
            if video is not None and shiboken6.isValid(video):
                video.setParent(self._shell)
                video.show()
                # the shell fitted the surface for its own geometry before the
                # float took it, and re-running its layout is the only public
                # way to fit the surface back into the picture
                self._shell.set_aspect_ratio(self._aspect)
            if not closing:
                window.close()
            window.deleteLater()
        if self._closed:
            return
        if not self.isVisible():
            self.show()
        if self._pip_was_fullscreen:
            self.showFullScreen()
        self._pip_was_fullscreen = False
        self._flash_status("Повернуто у вікно")

    def _sync_pip_bar(self, state: PlaybackState) -> None:
        if not self.pip_active:
            return
        pip = self._pip
        pip.set_playing(state.status in PLAYING_STATES)
        duration = fmt_time(state.duration) if state.duration > 0 else "--:--"
        pip.set_time_text(f"{fmt_time(state.position)} / {duration}")

    # ── shell extras ───────────────────────────────────────────────────

    def _open_stream_page(self) -> None:
        url = self._controller.current_url or self._url
        if url:
            QDesktopServices.openUrl(QUrl(url))

    def _copy_stream_url(self) -> None:
        """There is no file behind an HLS stream, so "download" means the link
        the user can actually save from."""
        url = self._controller.current_url or self._url
        if not url:
            return
        QGuiApplication.clipboard().setText(url)
        self._flash_status("Посилання на потік скопійовано")

    # ── events ─────────────────────────────────────────────────────────

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:
        if self._closed:
            return False
        if watched is self._shell and event.type() == QEvent.Type.KeyPress:
            return self._on_shell_key(event)
        pip = self._pip
        if (
            pip is not None
            and shiboken6.isValid(pip)
            and (watched is pip or (isinstance(watched, QWidget) and pip.isAncestorOf(watched)))
        ):
            if event.type() == QEvent.Type.MouseButtonDblClick:
                self._leave_pip()
                return True
            if event.type() == QEvent.Type.Close:
                # defer: tearing down and re-showing the player from inside the
                # float's own Close event re-enters close() on the same window
                QTimer.singleShot(0, lambda: self._leave_pip(closing=True))
                return True
        return super().eventFilter(watched, event)

    def _on_shell_key(self, event) -> bool:
        """Run ahead of the shell's own keyPressEvent so the panels get first
        refusal on Esc and own their keys; everything else falls through
        untouched, so Space, F, M and the arrows still reach the shell."""
        key = event.key()
        if key == Qt.Key.Key_Escape:
            if self._quality_popup.isVisible():
                self._quality_popup.close()
            elif self._track_popup.isVisible():
                self._track_popup.close()
            elif self._series_expanded:
                self.set_series_expanded(False)
            else:
                return False
            event.accept()
            return True
        if key == Qt.Key.Key_S and self._playlist:
            self.toggle_series()
        elif key == Qt.Key.Key_T and len(self._variants) > 1:
            self.open_track_menu()
        elif key == Qt.Key.Key_Q and self._qualities:
            self.open_quality_menu()
        else:
            return False
        event.accept()
        return True

    def closeEvent(self, event) -> None:
        self._closed = True
        self._status_timer.stop()
        self._quality_popup.close()
        self._track_popup.close()
        if self.pip_active:
            self._leave_pip(closing=True)
        if shiboken6.isValid(self._shell):
            self._shell.close()
        # the controller owns the libmpv handle and its thread: a dialog closed
        # by the title page's shutdown must not leave either behind
        self._controller.close()
        super().closeEvent(event)
