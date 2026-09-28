"""Composition root for the embedded Mikai player."""

from __future__ import annotations

import logging
from collections.abc import Callable
from pathlib import Path

import shiboken6
from PySide6.QtCore import QEvent, QLineF, QObject, QPoint, QSize, Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QDesktopServices, QGuiApplication, QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QAbstractButton,
    QApplication,
    QDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from mikai_ui.fullscreen_manager import FullscreenManager
from mikai_ui.theme.colors import ICON_DISABLED
from mikai_ui.mpv_backend import MPVAdapter
from mikai_ui.player_controller import PlaybackState, PlaybackStatus, PlayerController
from mikai_ui.pip_window import PipWindow
from mikai_ui.player_chrome import (
    FlankChevrons,
    PlayerFooter,
    SourcePill,
    PlayerDismissButton,
)
from mikai_ui.player_controls import PlayerControlsWidget, format_time
from mikai_ui.player_view import PAN_THRESHOLD_PX, ZOOM_STEP, Cursor as ViewCursor, ViewMachine
from mikai_ui.theme.widgets import make_icon
from mikai_ui.player_overlay import PlayerOverlayWidget
from mikai_ui.theme.metrics import (
    ICON_SIZE,
    PLAYER_DEFAULT_HEIGHT,
    PLAYER_DEFAULT_WIDTH,
    PIP_DEFAULT_HEIGHT,
    PIP_DEFAULT_WIDTH,
    PLAYER_MIN_HEIGHT,
    PLAYER_MIN_WIDTH,
    PLAYER_SIDEBAR_STRIP,
    PLAYER_SIDEBAR_WIDTH,
    SIDEBAR_CLOSE_DELAY_MS,
    SPACING_LARGE,
    SPACING_MEDIUM,
    SPACING_XS,
)

log = logging.getLogger(__name__)

# Qt's QWIDGETSIZE_MAX is a C++ macro that PySide6 does not export
UNLIMITED_HEIGHT = 16777215


class PlayerDialog(QDialog):
    """Own widget composition and connect user intent to PlayerController."""

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
        self._qualities = dict(qualities or {})
        self._variants = list(variants or [])
        self._current_embed = current_embed
        self._runtime_dir: Path | None = None
        self._closed = False
        self._showing = False
        self._refresh_attempted = False
        self._view = ViewMachine()
        self._pan_press: QPoint | None = None
        self._pan_cursor = QPoint()
        self._toolbar_floating = False

        self.setObjectName("playerWindow")
        self.setWindowTitle(f"Плеєр — {caption}")
        # No title bar: the chrome bar and the video are the whole window, the
        # same way the cover overlay drops every frame. The header, which only
        # shows outside fullscreen, becomes the drag handle so a windowed
        # player can still be moved.
        self.setWindowFlag(Qt.WindowType.FramelessWindowHint, True)
        self._drag_offset: QPoint | None = None
        self._pip: QWidget | None = None
        self._pip_was_fullscreen = False
        self.setMinimumSize(PLAYER_MIN_WIDTH, PLAYER_MIN_HEIGHT)
        self.resize(PLAYER_DEFAULT_WIDTH, PLAYER_DEFAULT_HEIGHT)
        self.setContentsMargins(0, 0, 0, 0)
        self.setMouseTracking(True)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        self._header = QWidget(self)
        self._header.setObjectName("playerHeader")
        header_layout = QHBoxLayout(self._header)
        header_layout.setContentsMargins(
            SPACING_LARGE, SPACING_MEDIUM, SPACING_LARGE, SPACING_MEDIUM
        )
        self._title = QLabel(caption, self._header)
        self._title.setObjectName("playerTitle")
        self._title.setWordWrap(False)
        self._title.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
        self._title.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        self._title.installEventFilter(self)
        header_layout.addWidget(self._title, 1)
        self._header.installEventFilter(self)
        root.addWidget(self._header)

        self._stage = QWidget(self)
        self._stage.setObjectName("videoStage")
        self._stage.setMouseTracking(True)
        root.addWidget(self._stage, 1)

        # The video surface and the chrome are laid out, never overlapped. A native
        # window stacked over the mpv surface paints opaque and hides the video, so
        # the controls get their own band and the video takes the rest.
        stage_layout = QVBoxLayout(self._stage)
        stage_layout.setContentsMargins(0, 0, 0, 0)
        stage_layout.setSpacing(0)

        # the video and the episode sidebar share a row: the sidebar is a sibling
        # of the native surface, never an overlay on it, so expanding it narrows
        # the video instead of covering it
        content_row = QHBoxLayout()
        content_row.setContentsMargins(0, 0, 0, 0)
        content_row.setSpacing(0)

        self._video = QWidget(self._stage)
        self._video.setObjectName("videoSurface")
        self._video.setAttribute(Qt.WidgetAttribute.WA_NativeWindow, True)
        self._video.setAttribute(Qt.WidgetAttribute.WA_DontCreateNativeAncestors, False)
        self._video.setMouseTracking(True)
        content_row.addWidget(self._video, 1)

        self._sidebar = self._build_sidebar()
        content_row.addWidget(self._sidebar, 0)

        self._window_controls = PlayerDismissButton(self._stage)
        self._window_controls.closeRequested.connect(self.close)
        self._chevrons = FlankChevrons(self._stage)
        self._chevrons.previousRequested.connect(lambda: self.step_episode(-1))
        self._chevrons.nextRequested.connect(lambda: self.step_episode(1))
        self._source_pill = SourcePill(self._stage)
        self._source_pill.linkClicked.connect(self._open_stream_in_browser)
        stage_layout.addLayout(content_row, 1)

        self._footer = PlayerFooter(self._stage)
        self._footer.openRequested.connect(self._open_stream_in_browser)
        self._footer.moreRequested.connect(self._copy_stream_url)
        footer_row = QHBoxLayout()
        footer_row.setContentsMargins(SPACING_LARGE, 0, SPACING_LARGE, 0)
        footer_row.addWidget(self._footer)
        stage_layout.addLayout(footer_row, 0)

        self._controls = PlayerControlsWidget(self._stage)
        self._controls.set_qualities(self._qualities, initial_height)
        self._controls.set_variants(self._variants, current_embed)
        self._controls.pipRequested.connect(self.toggle_pip)
        self._controls.variantRequested.connect(self._on_variant_requested)
        self._controls.variantMenuOpened.connect(self._probe_unknown_tracks)
        self._controls.prevEpisodeRequested.connect(lambda: self.step_episode(-1))
        self._controls.nextEpisodeRequested.connect(lambda: self.step_episode(1))
        # Telegram floats the controls as a rounded pill clear of the edges; a
        # full-width band pinned to the frame reads as chrome bolted to the
        # video. The wrapper carries the margins because a widget's own
        # contentsMargins only move its children, never its own slot in the
        # parent layout, so the bar would stay edge-to-edge.
        controls_row = QHBoxLayout()
        controls_row.setContentsMargins(SPACING_LARGE, 0, SPACING_LARGE, SPACING_MEDIUM)
        controls_row.addWidget(self._controls)
        stage_layout.addLayout(controls_row, 0)
        self._controls_row = controls_row
        self.set_playlist(playlist, episode_index)

        # The overlay is an event filter and state presenter only. It must never be
        # given the stage rect: any widget spanning the video occludes the mpv surface.
        self._overlay = PlayerOverlayWidget(self._controls, self._stage)
        self._overlay.chromeHidden.connect(self._sidebar.hide)
        self._overlay.mouseActivity.connect(self._sidebar.show)
        self._overlay.set_mouse_sources(self._stage, self._video)
        self._overlay.hide()

        # The status card and the toast are small opaque panels that do float over
        # the video, so each earns a small native handle. What must never happen is a
        # full-stage native window over the video: it paints opaque and hides it.
        for panel in (
            self._overlay.status_card,
            self._overlay.toast,
            self._overlay.stream_chip,
            self._chevrons,
            self._source_pill,
            self._window_controls,
        ):
            panel.setParent(self._stage)
            panel.setAttribute(Qt.WidgetAttribute.WA_NativeWindow, True)
            panel.raise_()
        self._overlay.toastShown.connect(self._layout_panels)
        self._overlay.panelsChanged.connect(self._layout_panels)

        # Compatibility aliases for code that used to inspect the dialog's UI.
        self._chrome = self._controls
        self._timeline = self._controls.timeline
        self._time_label = self._controls.time_label
        self._pause_btn = self._controls.play_button
        self._back_btn = self._controls.rewind_button
        self._fwd_btn = self._controls.forward_button
        self._mute_btn = self._controls.mute_button
        self._vol_slider = self._controls.volume_slider
        self._quality_picker = self._controls.quality_picker

        self._controller = PlayerController(
            url,
            self._qualities,
            initial_height,
            self,
        )
        self._controller.stateChanged.connect(self._on_state_changed)
        self._controller.qualitySwitching.connect(self._overlay.show_quality_notice)
        self._controller.seekFeedback.connect(self._overlay.show_seek_feedback)
        self._controller.seekFeedback.connect(self._show_controls)
        self._fullscreen = FullscreenManager(self)
        self._fullscreen.fullscreenChanged.connect(self._on_fullscreen_changed)
        self._connect_controls()
        self._overlay.videoClicked.connect(self._controller.toggle_play_pause)
        self._overlay.retryRequested.connect(self._retry_playback)
        self._overlay.detailsRequested.connect(self._show_error_details)

        self._install_shortcuts()
        self._install_canvas_input()
        self._view.subscribe(self._apply_view)
        self._overlay.mouseActivity.connect(self._view.wake)
        self._overlay.chromeHidden.connect(self._view.hide_chrome)
        self._on_state_changed(self._controller.state)
        self._sync_source_pill()
        self._sync_stage_geometry()

    @property
    def controller(self) -> PlayerController:
        return self._controller

    @property
    def status_text(self) -> str:
        return self._overlay.status_title.text()

    @staticmethod
    def format_time(seconds: float) -> str:
        return format_time(seconds)

    def _connect_controls(self) -> None:
        controls = self._controls
        controller = self._controller
        controls.playPauseRequested.connect(controller.toggle_play_pause)
        controls.rewindRequested.connect(lambda: controller.rewind(10))
        controls.forwardRequested.connect(lambda: controller.forward(10))
        controls.seekStarted.connect(controller.begin_seek)
        controls.seekPreviewChanged.connect(controller.preview_seek)
        controls.seekFinished.connect(controller.finish_seek)
        controls.volumeChanged.connect(controller.set_volume)
        controls.muteRequested.connect(controller.toggle_mute)
        controls.speedChanged.connect(controller.set_speed)
        controls.qualityChanged.connect(controller.set_quality)
        controls.fullscreenRequested.connect(self._fullscreen.toggle)

    def _install_shortcuts(self) -> None:
        shortcuts = (
            ("Space", self._toggle_play_pause),
            ("Left", lambda: self._controller.rewind(10)),
            ("Right", lambda: self._controller.forward(10)),
            ("F", self._fullscreen.toggle),
            ("M", self._controller.toggle_mute),
            ("Up", lambda: self._change_volume(5)),
            ("Down", lambda: self._change_volume(-5)),
            ("Esc", self._escape),
            ("PgUp", lambda: self.step_episode(-1)),
            ("PgDown", lambda: self.step_episode(1)),
            ("E", self.toggle_sidebar),
            ("P", self.toggle_pip),
        )
        self._shortcuts: list[QShortcut] = []
        for key, callback in shortcuts:
            shortcut = QShortcut(QKeySequence(key), self)
            shortcut.setContext(Qt.ShortcutContext.WindowShortcut)
            shortcut.activated.connect(lambda command=callback: self._handle_shortcut(command))
            self._shortcuts.append(shortcut)

    def _handle_shortcut(self, command: Callable[[], None]) -> None:
        self._overlay.show_controls()
        command()

    def _escape(self) -> None:
        """Undo one thing at a time: the float, then fullscreen, then the player.

        The last step is the cover viewer's dismiss idiom. With the window
        frame gone and the minimise button removed, Esc is the keyboard way out.
        """
        if self.pip_active:
            self._leave_pip()
            return
        if self._fullscreen.is_fullscreen:
            self._fullscreen.exit()
            return
        self.close()

    def _toggle_play_pause(self) -> None:
        focus = self.focusWidget()
        if isinstance(focus, QAbstractButton):
            return
        self._controller.toggle_play_pause()

    def _change_volume(self, delta: int) -> None:
        self._controller.set_volume(self._controller.state.volume + delta)

    def _exit_fullscreen(self) -> None:
        if self._fullscreen.is_fullscreen:
            self._fullscreen.exit()

    def start_player(self, runtime_dir: Path | None = None) -> None:
        if self._closed:
            return
        if self._controller.adapter is not None:
            return
        self._runtime_dir = Path(runtime_dir) if runtime_dir is not None else None
        try:
            adapter = MPVAdapter(self._video, mpv_path=self._runtime_dir)
            self._controller.attach_adapter(adapter)
            self._controller.load()
        except Exception as exc:
            log.exception("player startup failed")
            self._controller.fail(str(exc))

    def show_error(self, message: str) -> None:
        self._controller.fail(message)

    def _build_sidebar(self) -> QWidget:
        """A collapsible episode list beside the video, not over it."""
        sidebar = QWidget(self._stage)
        sidebar.setObjectName("playerSidebar")
        column = QVBoxLayout(sidebar)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(0)

        self._sidebar_toggle = QPushButton(sidebar)
        self._sidebar_toggle.setObjectName("playerSidebarToggle")
        self._sidebar_toggle.setIcon(
            make_icon("chevron_right", size=ICON_SIZE, disabled_color=ICON_DISABLED)
        )
        self._sidebar_toggle.setToolTip("Список серій (E)")
        self._sidebar_toggle.setAccessibleName("Список серій")
        # a focused button would swallow Space, and Space is play/pause
        self._sidebar_toggle.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        # the whole collapsed rail is the button: a 28px square at the top left
        # most of it dead, so a click below the chevron did nothing
        self._sidebar_toggle.setSizePolicy(
            QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Expanding
        )
        self._sidebar_toggle.setFixedWidth(PLAYER_SIDEBAR_STRIP)
        self._sidebar_toggle.setMaximumHeight(UNLIMITED_HEIGHT)
        self._sidebar_toggle.setIconSize(QSize(ICON_SIZE, ICON_SIZE))
        self._sidebar_toggle.clicked.connect(self.toggle_sidebar)
        column.addWidget(self._sidebar_toggle, 0)

        self._sidebar_list = QWidget(sidebar)
        self._sidebar_list.setObjectName("playerSidebarList")
        self._sidebar_items = QVBoxLayout(self._sidebar_list)
        self._sidebar_items.setContentsMargins(SPACING_XS, SPACING_XS, SPACING_XS, SPACING_XS)
        self._sidebar_items.setSpacing(2)
        self._sidebar_items.addStretch(1)
        self._sidebar_scroll = QScrollArea(sidebar)
        self._sidebar_scroll.setObjectName("playerSidebarScroll")
        self._sidebar_scroll.setWidgetResizable(True)
        self._sidebar_scroll.setFrameShape(QFrame.Shape.NoFrame)
        self._sidebar_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._sidebar_scroll.setWidget(self._sidebar_list)
        self._sidebar_scroll.setFixedWidth(PLAYER_SIDEBAR_WIDTH)
        column.addWidget(self._sidebar_scroll, 1)
        self._sidebar_expanded = False
        self._sidebar_pinned = False
        self._sidebar_hovered = False
        self._sidebar_close_timer = QTimer(self)
        self._sidebar_close_timer.setSingleShot(True)
        self._sidebar_close_timer.setInterval(SIDEBAR_CLOSE_DELAY_MS)
        self._sidebar_close_timer.timeout.connect(self._close_sidebar_after_grace)
        for child in (sidebar, *sidebar.findChildren(QWidget)):
            child.setMouseTracking(True)
            child.installEventFilter(self)
        return sidebar

    def set_playlist(self, playlist: list[dict] | None, index: int = 0) -> None:
        """The episodes around this one, for the sidebar and the step buttons."""
        self._playlist = [dict(item) for item in (playlist or [])]
        self._episode_index = index if 0 <= index < len(self._playlist) else 0
        self._rebuild_sidebar_rows()
        self.set_sidebar_expanded(self._sidebar_expanded)
        self._sync_episode_state()

    def _sync_episode_state(self) -> None:
        has_previous = self._episode_index > 0
        has_next = self._episode_index < len(self._playlist) - 1
        self._controls.set_episode_navigation(has_previous, has_next)
        self._chevrons.set_available(has_previous, has_next)

    def step_episode(self, delta: int) -> None:
        self._go_to_episode(self._episode_index + delta)

    def _go_to_episode(self, position: int, *, close_list: bool = False) -> None:
        """Step or jump to an episode.

        Picking from the list closes it, because that means "this one, now"; the
        transport buttons leave it open, since stepping while browsing a long
        list and having it snap shut under the pointer is a fight.
        """
        if not 0 <= position < len(self._playlist):
            return
        if close_list:
            self.set_sidebar_expanded(False)
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

        The stream swap reuses the variant machine, so playback continues where
        it was instead of restarting, and a new dialog is never stacked.
        """
        if self._closed or not qualities:
            return
        self._episode_index = index
        self._variants = list(variants)
        self._current_embed = embed
        self._controls.set_variants(self._variants, embed)
        target = max(qualities)
        self._qualities = dict(qualities)
        self._controls.set_qualities(self._qualities, target)
        self._caption = f"{label} · {target}p"
        self.setWindowTitle(f"Плеєр — {self._caption}")
        self._rebuild_sidebar_rows()
        self._sync_episode_state()
        self._overlay.show_toast(f"Серія: {label}")
        self._sync_source_pill()
        self._controller.switch_variant(self._qualities[target], self._qualities, target)

    def _rebuild_sidebar_rows(self) -> None:
        while self._sidebar_items.count() > 1:
            item = self._sidebar_items.takeAt(0)
            widget = item.widget()
            if widget is not None:
                widget.deleteLater()
        for position, entry in enumerate(self._playlist):
            button = QPushButton(entry.get("label", ""), self._sidebar_list)
            button.setObjectName(
                "playerEpisodeItemCurrent"
                if position == self._episode_index
                else "playerEpisodeItem"
            )
            button.setCursor(Qt.CursorShape.PointingHandCursor)
            button.setToolTip(entry.get("label", ""))
            button.clicked.connect(
                lambda _checked=False, spot=position: self._go_to_episode(spot, close_list=True)
            )
            self._sidebar_items.insertWidget(position, button)

    def toggle_sidebar(self) -> None:
        """Hover opens the list; the click pins it open or closed."""
        self._sidebar_pinned = not self._sidebar_pinned
        self._sidebar_close_timer.stop()
        self.set_sidebar_expanded(self._sidebar_pinned or self._sidebar_hovered)

    def _close_sidebar_after_grace(self) -> None:
        if not self._sidebar_pinned and not self._sidebar_hovered:
            self.set_sidebar_expanded(False)

    def set_sidebar_expanded(self, expanded: bool) -> None:
        """Open or close the list.

        Hover is the fast path: pointing at the rail is enough, and no click is
        demanded. A click pins the list open so it survives the pointer leaving,
        and collapses it back to hover-only.
        """
        self._sidebar_expanded = bool(expanded)
        self._sidebar_scroll.setVisible(self._sidebar_expanded)
        # collapsed: the rail fills the height; expanded: a header above the list
        self._sidebar_toggle.setMaximumHeight(
            PLAYER_SIDEBAR_STRIP if self._sidebar_expanded else UNLIMITED_HEIGHT
        )
        self._sidebar_toggle.setIcon(
            make_icon(
                "chevron_left" if self._sidebar_expanded else "chevron_right",
                size=ICON_SIZE,
                disabled_color=ICON_DISABLED,
            )
        )
        hint = "Сховати список серій" if self._sidebar_expanded else "Список серій"
        self._sidebar_toggle.setToolTip(hint)
        self._sidebar_toggle.setAccessibleName(hint)
        self._sidebar.setFixedWidth(
            (PLAYER_SIDEBAR_STRIP + PLAYER_SIDEBAR_WIDTH)
            if self._sidebar_expanded
            else PLAYER_SIDEBAR_STRIP
        )
        # called during construction, before the overlay exists
        overlay = getattr(self, "_overlay", None)
        if overlay is not None and not overlay.controls.isVisible():
            self._sidebar.hide()

    def _sidebar_hover_changed(self, inside: bool) -> None:
        """Open on hover, but close on a delay.

        Collapsing the instant the pointer leaves makes the list unreachable:
        the rows sit to the right of the rail, so a pointer heading for one
        crosses out and the list vanishes under it. The grace period lets it
        settle first, and re-entering during it cancels the close outright.
        """
        self._sidebar_hovered = inside
        if self._sidebar_pinned:
            return
        if inside:
            self._sidebar_close_timer.stop()
            if not self._sidebar_expanded:
                self.set_sidebar_expanded(True)
        elif self._sidebar_expanded:
            self._sidebar_close_timer.start()

    def _probe_unknown_tracks(self) -> None:
        """Ask for the qualities the menu could not show yet.

        Lazy on purpose: a track that is never looked at never costs a request,
        and one already cached costs nothing.
        """
        if self._closed:
            return
        for embed in self._controls.pending_track_probes():
            if embed != self._current_embed:
                self.trackProbeRequested.emit(embed)

    def set_track_quality(self, embed: str, quality: int) -> None:
        """Badge one track with the quality a probe found for it."""
        if self._closed or quality <= 0:
            return
        for variant in self._variants:
            if variant.get("embed") == embed:
                variant["quality"] = quality
        self._controls.set_variants(self._variants, self._current_embed)

    def _on_variant_requested(self, embed: str, label: str) -> None:
        if embed and embed != self._current_embed:
            self.variantRequested.emit(embed, label)

    def apply_variant(self, qualities: dict[int, str], label: str, embed: str = "") -> None:
        """Swap the dub/sub track; the controller resumes at the same position."""
        if self._closed or not qualities:
            return
        if embed:
            self._current_embed = embed
            self._controls.set_variants(self._variants, embed)
        target = max(qualities)
        self._qualities = dict(qualities)
        self._controls.set_qualities(self._qualities, target)
        self._overlay.show_toast(f"Дорожка: {label}")
        self._sync_source_pill()
        self._controller.switch_variant(self._qualities[target], self._qualities, target)

    def apply_refreshed_streams(
        self,
        qualities: dict[int, str],
        height: int | None = None,
    ) -> None:
        """Reload from a re-extracted quality map after a failed stream load."""
        if self._closed or not qualities:
            return
        target = height if height in qualities else max(qualities)
        self._qualities = dict(qualities)
        self._controls.set_qualities(self._qualities, target)
        self._overlay.show_toast("Посилання на потік прострочене — оновлюю")
        self._sync_source_pill()
        self._controller.replace_stream(self._qualities[target], self._qualities)

    def _retry_playback(self) -> None:
        if self._controller.adapter is None:
            self.runtimeRequested.emit()
        else:
            self._controller.retry()

    def _show_error_details(self) -> None:
        details = self._overlay.details_text().strip()
        box = QMessageBox(self)
        box.setIcon(QMessageBox.Icon.Warning)
        box.setWindowTitle("Деталі відтворення")
        box.setText("Плеєр не зміг відкрити цей потік.")
        box.setDetailedText(details or "Технічних деталей немає.")
        box.exec()

    def _on_state_changed(self, state: PlaybackState) -> None:
        if self._closed:
            return
        self._overlay.set_playback_state(state)
        self._sync_pip_bar(state)
        self._layout_panels()
        if state.status is PlaybackStatus.ERROR and not self._refresh_attempted:
            self._refresh_attempted = True
            self.playbackFailed.emit()

    def _on_fullscreen_changed(self, fullscreen: bool) -> None:
        # the view owns the state matrix; the overlay and the bar still own their
        # own fullscreen reaction (the idle fade, and the header that is windowed-only)
        self._header.setVisible(not fullscreen)
        self._controls.set_fullscreen(fullscreen)
        self._overlay.set_fullscreen(fullscreen)
        if fullscreen:
            self._view.enter_fullscreen()
        else:
            self._view.exit_fullscreen()
        self._sync_stage_geometry()
        QTimer.singleShot(0, self._sync_stage_geometry)

    def _show_controls(self) -> None:
        self._overlay.show_controls()

    def _sync_pip_bar(self, state: PlaybackState) -> None:
        """Keep the float's own play glyph and clock in step with the engine."""
        if not self.pip_active:
            return
        pip = self._pip
        pip.set_playing(not state.paused and state.status is not PlaybackStatus.ERROR)
        duration = format_time(state.duration) if state.duration > 0 else "--:--"
        pip.set_time_text(f"{format_time(state.position)} / {duration}")

    @property
    def pip_active(self) -> bool:
        return self._pip is not None and shiboken6.isValid(self._pip)

    def toggle_pip(self) -> None:
        """Float the video in a borderless always-on-top window.

        The SAME video widget is reparented rather than a second mpv instance:
        one decoder and one position means the float can be opened and closed
        mid-playback without re-seeking, buffering or drifting.
        """
        if self._closed or self._video is None:
            return
        if self.pip_active:
            self._leave_pip()
        else:
            self._enter_pip()

    def _enter_pip(self) -> None:
        # The main player must disappear completely, not linger as a black
        # frame: the video now belongs to the float, so what is left behind is
        # an empty stage. Remember fullscreen so returning restores it.
        self._pip_was_fullscreen = self._fullscreen.is_fullscreen
        if self._pip_was_fullscreen:
            self._fullscreen.exit()
        # Parentless on purpose. A child window is clipped to its parent's
        # z-order, so a float parented to the player slides UNDER whatever app
        # the user switches to; an independent top-level window is what actually
        # stays above other applications.
        window = PipWindow()
        window.togglePlayRequested.connect(self._controller.toggle_play_pause)
        window.seekRequested.connect(
            lambda delta: self._controller.seek(self._controller.state.position + delta)
        )
        window.closeRequested.connect(self._leave_pip)
        window.set_video(self._video)
        window.resize(PIP_DEFAULT_WIDTH, PIP_DEFAULT_HEIGHT)
        # keep it near the corner the main window already occupies
        screen = self.screen() or QApplication.primaryScreen()
        if screen is not None:
            area = screen.availableGeometry()
            window.move(
                area.right() - PIP_DEFAULT_WIDTH - 24, area.bottom() - PIP_DEFAULT_HEIGHT - 24
            )
        self._pip = window
        # Close, not destroyed: close() on a Qt window only hides it, so the
        # destroyed signal never fires and the video would stay parented to an
        # invisible float, leaving the main player with a black hole.
        window.installEventFilter(self)
        window.show()
        self._sync_pip_bar(self._controller.state)
        self.hide()
        self._controls.pip_button.setToolTip("Повернути в вікно (P)")
        self._overlay.show_toast("Картинка в картинці")

    def _leave_pip(self, *, closing: bool = False) -> None:
        """Take the video back and bring the player window home.

        `closing` means the float is already inside its own Close event, so it
        must not be closed again from here.
        """
        window, self._pip = self._pip, None
        if window is not None and shiboken6.isValid(window):
            video = window.release_video()
            if video is not None and shiboken6.isValid(video):
                video.setParent(self._stage)
                video.show()
            if not closing:
                window.close()
            window.deleteLater()
        self.show()
        if self._pip_was_fullscreen:
            self._fullscreen.enter()
        self._pip_was_fullscreen = False
        QTimer.singleShot(0, self._sync_stage_geometry)
        self._controls.pip_button.setToolTip("Картинка в картинці (P)")
        self._overlay.show_toast("Повернуто у вікно")

    def _drag_window(self, event) -> None:
        """Move a windowed, frameless player by its header."""
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if self._drag_offset is None:
            self._drag_offset = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
        self.move(event.globalPosition().toPoint() - self._drag_offset)
        event.accept()

    def _end_drag(self, event) -> None:
        self._drag_offset = None

    def show(self) -> None:
        # Open in fullscreen for the focused player experience, while the
        # manager retains the dialog geometry for reliable restoration.
        super().show()
        if not self._showing and not self._fullscreen.is_fullscreen:
            self._showing = True
            try:
                self._fullscreen.enter()
            finally:
                self._showing = False

    def changeEvent(self, event: QEvent) -> None:
        super().changeEvent(event)
        if event.type() == QEvent.Type.WindowStateChange:
            self._on_fullscreen_changed(self.isFullScreen())

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:
        # Eliding here rather than in resizeEvent: the layout has not yet given the
        # label its new width when the dialog resizes, and a wrapped header would
        # steal height from the video.
        if self._closed:
            return False
        if watched is getattr(self, "_video", None) or watched is getattr(self, "_stage", None):
            if self._canvas_event(event):
                return True

        pip = getattr(self, "_pip", None)
        if (
            pip is not None
            and shiboken6.isValid(pip)
            and (watched is pip or (isinstance(watched, QWidget) and pip.isAncestorOf(watched)))
        ):
            if event.type() == QEvent.Type.MouseButtonDblClick:
                self._leave_pip()
                return True
            if event.type() == QEvent.Type.Close:
                # Defer: tearing down and re-showing the player from inside the
                # float's own Close event re-enters close() on the same window,
                # which is an access violation. Let the event finish first.
                QTimer.singleShot(0, lambda: self._leave_pip(closing=True))
                return True

        header = getattr(self, "_header", None)
        if header is not None and (
            watched is header or (isinstance(watched, QWidget) and header.isAncestorOf(watched))
        ):
            if event.type() == QEvent.Type.MouseButtonPress:
                self._drag_window(event)
                return True
            if event.type() == QEvent.Type.MouseButtonRelease:
                self._end_drag(event)
                return True
        if watched is self._title and event.type() == QEvent.Type.Resize:
            metrics = self._title.fontMetrics()
            self._title.setText(
                metrics.elidedText(self._caption, Qt.TextElideMode.ElideRight, self._title.width())
            )
        sidebar = getattr(self, "_sidebar", None)
        if sidebar is not None and self._within(sidebar, watched):
            if event.type() == QEvent.Type.Enter:
                self._sidebar_hover_changed(True)
            elif event.type() == QEvent.Type.Leave:
                self._sidebar_hover_changed(False)
            return False
        return super().eventFilter(watched, event)

    @staticmethod
    def _within(panel: QWidget, watched: QObject) -> bool:
        """Ancestry test that survives a panel deleted mid-flight."""
        try:
            return watched is panel or panel.isAncestorOf(watched)
        except RuntimeError, AttributeError:
            return False

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._sync_stage_geometry()

    def _layout_panels(self) -> None:
        area = self._stage.rect()

        def place(widget: QWidget, top: int) -> None:
            hint = widget.sizeHint()
            width = min(area.width() - 2 * SPACING_LARGE, max(hint.width(), 1))
            height = max(hint.height(), 1)
            widget.setGeometry(area.x() + (area.width() - width) // 2, top, width, height)

        if self._overlay.status_card.isVisible():
            place(self._overlay.status_card, area.y() + max(SPACING_LARGE, area.height() // 4))
        if self._overlay.stream_chip.isVisible():
            chip = self._overlay.stream_chip
            chip_hint = chip.sizeHint()
            chip.setGeometry(
                area.x() + SPACING_LARGE,
                area.y() + SPACING_LARGE,
                chip_hint.width(),
                chip_hint.height(),
            )
        if self._window_controls.isVisible():
            hint = self._window_controls.sizeHint()
            self._window_controls.setGeometry(
                area.x() + area.width() - hint.width() - SPACING_MEDIUM,
                area.y() + SPACING_MEDIUM,
                hint.width(),
                hint.height(),
            )
        if self._chevrons.isVisible():
            self._chevrons.setGeometry(area.x(), area.y(), area.width(), area.height())
        if self._source_pill.isVisible():
            # the pill hangs off the picture, not off the stage: the stage bottom
            # is the dock, so a stage-anchored pill lands on top of it
            pill = self._source_pill
            pill_hint = pill.sizeHint()
            frame = self._video.geometry()
            pill.setGeometry(
                frame.x() + (frame.width() - pill_hint.width()) // 2,
                frame.y() + frame.height() - pill_hint.height() - SPACING_MEDIUM,
                pill_hint.width(),
                pill_hint.height(),
            )
        self._position_floating_toolbar()
        if self._overlay.toast.isVisible():
            # The toast used to be centred like the status card, so a quality switch
            # during an error hid the message behind it. It belongs on the control bar.
            bar_top = (
                area.y()
                + area.height()
                - (self._controls.height() if self._controls.isVisible() else 0)
            )
            place(
                self._overlay.toast,
                bar_top - self._overlay.toast.sizeHint().height() - SPACING_MEDIUM,
            )
        for panel in (
            self._overlay.status_card,
            self._overlay.toast,
            self._overlay.stream_chip,
            self._source_pill,
        ):
            if panel.isVisible():
                panel.raise_()

    def _sync_stage_geometry(self) -> None:
        if not hasattr(self, "_stage"):
            return
        self._layout_panels()

    def _install_canvas_input(self) -> None:
        """Wheel, double click, pinch and drag all land on the picture itself.

        The filters go on last so they run before the overlay's, which is what
        turns a click into play/pause. Returning False on a plain press leaves
        that path intact; only a real drag swallows the event.
        """
        self._stage.setAttribute(Qt.WidgetAttribute.WA_AcceptTouchEvents, True)
        for widget in (self._stage, self._video):
            widget.installEventFilter(self)

    def _canvas_event(self, event: QEvent) -> bool:
        kind = event.type()
        if kind == QEvent.Type.Wheel:
            delta = event.angleDelta().y()
            if not delta:
                return False
            self._view.zoom_by(ZOOM_STEP if delta > 0 else -ZOOM_STEP)
            self._view.wake()
            event.accept()
            return True
        if kind == QEvent.Type.Gesture:
            return self._pinch_event(event)
        if kind == QEvent.Type.MouseButtonDblClick:
            self._view.toggle_magnify()
            self._view.wake()
            event.accept()
            return True
        if kind == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.LeftButton:
            self._pan_press = event.position().toPoint()
            self._pan_cursor = self._pan_press
            return False
        if kind == QEvent.Type.MouseMove and self._pan_press is not None:
            return self._pan_move(event)
        if kind == QEvent.Type.MouseButtonRelease and self._pan_press is not None:
            self._pan_press = None
            self._view.set_panning(False)
            self._pan_cursor = QPoint()
            self._apply_cursor(self._view.chrome.cursor)
            return False
        return False

    def _pan_move(self, event) -> bool:
        here = event.position().toPoint()
        if not self._view.panning:
            if QLineF(self._pan_press, here).length() < PAN_THRESHOLD_PX:
                return False
            self._view.set_panning(True)
            # the press already armed a play/pause click; a drag is not a click
            self._overlay.cancel_pending_click()
            self._apply_cursor(self._view.chrome.cursor)
        rect = self._video.rect()
        self._view.pan_by(
            (here.x() - self._pan_cursor.x()) / max(1, rect.width()),
            (here.y() - self._pan_cursor.y()) / max(1, rect.height()),
        )
        self._pan_cursor = here
        event.accept()
        return True

    def _pinch_event(self, event: QEvent) -> bool:
        gesture = event.gesture(Qt.GestureType.PinchGesture)
        if gesture is None:
            return False
        if gesture.state() in (Qt.GestureState.Began, Qt.GestureState.Changed):
            factor = gesture.scaleFactor()
            if factor > 0:
                self._view.set_scale(self._view.scale * factor)
        event.accept()
        return True

    def _apply_view(self, view: ViewMachine) -> None:
        if self._closed:
            return
        chrome = view.chrome
        self._controller.apply_canvas(view.scale, view.pan[0], view.pan[1])
        self._window_controls.setVisible(chrome.window_controls)
        self._chevrons.setVisible(chrome.gallery_navigation)
        windowed = not view.fullscreen
        self._source_pill.setVisible(chrome.source_badge and windowed)
        self._footer.setVisible(chrome.metadata_footer and windowed)
        self._set_toolbar_floating(chrome.floating_toolbar)
        if not chrome.playback_toolbar:
            # a popover anchored to a bar that is no longer there is a stray
            # panel left floating over the picture
            self._controls.speed_flyout.dismiss()
        self._apply_cursor(chrome.cursor)
        self._layout_panels()

    def _apply_cursor(self, cursor: ViewCursor) -> None:
        """Set on the video, not just the stage.

        The surface is a native child window with its own cursor, so a shape set
        on the parent never reaches the one the pointer is actually over.
        """
        if cursor in (ViewCursor.GRAB, ViewCursor.ZOOM):
            shape = (
                Qt.CursorShape.ClosedHandCursor
                if self._view.panning
                else Qt.CursorShape.OpenHandCursor
            )
        elif cursor is ViewCursor.NONE:
            shape = Qt.CursorShape.BlankCursor
        else:
            shape = Qt.CursorShape.ArrowCursor
        for widget in (self._video, self._stage):
            widget.setCursor(shape)

    def _set_toolbar_floating(self, floating: bool) -> None:
        """Lift the dock off its band and over the picture while magnified.

        It becomes a native panel, like the toast and the chip: a plain widget
        spanning the mpv surface would be composited on top and hide the video.
        """
        if floating == self._toolbar_floating:
            return
        self._toolbar_floating = floating
        if floating:
            self._controls_row.removeWidget(self._controls)
            self._controls.setParent(self._stage)
            self._controls.setAttribute(Qt.WidgetAttribute.WA_NativeWindow, True)
            self._controls.raise_()
        else:
            # Qt never gives a created native handle back, so the flag stays set.
            # That is harmless: a bar that sits in its own row never spans the
            # video, which is the only thing the handle must never do.
            self._controls.setParent(None)
            self._controls_row.insertWidget(0, self._controls)

    def _position_floating_toolbar(self) -> None:
        if not self._toolbar_floating:
            return
        area = self._stage.rect()
        hint = self._controls.sizeHint()
        width = min(area.width() - 2 * SPACING_LARGE, max(hint.width(), 1))
        self._controls.setGeometry(
            area.x() + (area.width() - width) // 2,
            area.y() + area.height() - max(hint.height(), 1) - SPACING_MEDIUM,
            width,
            max(hint.height(), 1),
        )

    def _open_stream_in_browser(self) -> None:
        if self._url:
            QDesktopServices.openUrl(QUrl(self._url))

    def _copy_stream_url(self) -> None:
        if not self._url:
            return
        QGuiApplication.clipboard().setText(self._url)
        self._overlay.show_toast("Посилання на потік скопійовано")

    def _track_source(self) -> str:
        """The release and team the current picture came from, if known."""
        for variant in self._variants:
            if variant.get("embed") != self._current_embed:
                continue
            parts = [variant.get("display"), variant.get("track")]
            return " · ".join(part for part in parts if part)
        return ""

    def _sync_source_pill(self) -> None:
        """Name what is playing, and where the picture came from.

        There is no original file behind a stream, so the pill carries the
        height instead of a filename, and the footer carries the episode with
        its release. They are kept distinct: printing one string twice reads as
        a rendering fault.
        """
        source = self._track_source()
        height = self._controls.quality_picker.selected_height() or max(self._qualities, default=0)
        self._source_pill.set_source(source, f"{height}p" if height else "")
        self._footer.set_metadata(self._caption, source or self._current_embed)

    def closeEvent(self, event) -> None:
        self._closed = True
        self._controls.speed_flyout.close()
        # a tool window outlives its parent by default and then holds the active
        # popup slot, breaking every later menu and tooltip
        if self.pip_active:
            self._leave_pip()
        self._controller.close()
        super().closeEvent(event)
