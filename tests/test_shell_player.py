"""The shell player driven the way a viewer drives it: transport, the two unit
boundaries between the shell and mpv, and the three panels mikai cannot lose.
"""

from __future__ import annotations

import pytest
from PySide6.QtCore import QEvent, Qt
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import QApplication, QPushButton

from conftest import FakeMpvAdapter

from mikai_ui.media_shell import STATE_FULLSCREEN_ACTIVE
from mikai_ui.player_controller import PlaybackStatus
from mikai_ui.shell_player import SHELL_RATE_MAX, ShellPlayerDialog


@pytest.fixture(autouse=True)
def _reap_players():
    """Close whatever a test left open.

    A dialog owns native panels and two Qt.Popup top-levels. Left alive, they
    outlive the session-scoped QApplication and the interpreter tears them down
    in an arbitrary order, which crashes about one run in four — after pytest has
    already reported success, so the suite looks green while the process dies.
    """
    yield
    app = QApplication.instance()
    if app is None:
        return
    stale = [w for w in app.topLevelWidgets() if isinstance(w, ShellPlayerDialog)]
    for widget in stale:
        widget.close()
        widget.deleteLater()
    if stale:
        # deleteLater only queues; without this the dialog and its native panels
        # are still alive when the session QApplication goes, and the teardown
        # order is then undefined
        app.sendPostedEvents(None, QEvent.Type.DeferredDelete)


STREAM = "https://s.example/1080.m3u8"
QUALITIES = {1080: "u1080", 720: "u720", 480: "u480"}
VARIANTS = [
    {
        "kind": "dub",
        "kind_label": "Озвучення",
        "display": "Ashdi",
        "track": "Dub",
        "label": "Ashdi · Dub",
        "embed": "dub",
        "quality": 0,
    },
    {
        "kind": "sub",
        "kind_label": "Субтитри",
        "display": "Ashdi",
        "track": "Sub",
        "label": "Ashdi · Sub",
        "embed": "sub",
        "quality": 0,
    },
]
PLAYLIST = [{"label": f"Серія {n}", "embed": f"e{n}"} for n in range(1, 6)]


def _build() -> ShellPlayerDialog:
    return ShellPlayerDialog(
        "Серія 3 · 1080p",
        STREAM,
        qualities=dict(QUALITIES),
        initial_height=1080,
        variants=[dict(variant) for variant in VARIANTS],
        current_embed="dub",
        playlist=[dict(item) for item in PLAYLIST],
        episode_index=2,
    )


def _key(app: QApplication, widget, key) -> None:
    app.sendEvent(widget, QKeyEvent(QKeyEvent.Type.KeyPress, key, Qt.KeyboardModifier.NoModifier))
    app.processEvents()


@pytest.fixture
def player(app):
    dialog = _build()
    adapter = FakeMpvAdapter()
    dialog.controller.attach_adapter(adapter)
    dialog.controller.load()
    adapter.durationChanged.emit(600.0)
    adapter.positionChanged.emit(30.0)
    adapter.mediaLoaded.emit()
    dialog.show()
    app.processEvents()
    yield dialog
    dialog.close()
    app.processEvents()


def _rows(popup) -> list[QPushButton]:
    return popup.findChildren(QPushButton, "playerMenuRow")


# --- opening ---


def test_the_dialog_opens_and_plays_once_the_engine_reports_a_file(app) -> None:
    dialog = _build()
    adapter = FakeMpvAdapter()
    dialog.show()
    app.processEvents()
    assert dialog.controller.state.status is PlaybackStatus.LOADING

    dialog.controller.attach_adapter(adapter)
    dialog.controller.load()
    assert adapter.loads == [STREAM]
    adapter.durationChanged.emit(600.0)
    adapter.mediaLoaded.emit()
    app.processEvents()

    assert dialog.controller.state.status is PlaybackStatus.PLAYING
    assert dialog._shell._duration == 600.0
    assert dialog._shell._quality == "1080p"
    dialog.close()
    app.processEvents()


def test_nothing_is_fed_to_the_shell_before_the_engine_knows_the_length(app) -> None:
    dialog = _build()
    durations: list[float] = []
    positions: list[float] = []
    dialog._shell.set_duration = durations.append
    dialog._shell.set_position = positions.append

    adapter = FakeMpvAdapter()
    dialog.controller.attach_adapter(adapter)
    adapter.positionChanged.emit(12.0)
    app.processEvents()
    assert durations == [] and positions == []

    adapter.durationChanged.emit(600.0)
    app.processEvents()
    assert durations == [600.0]
    assert positions == [12.0]
    dialog.close()
    app.processEvents()


# --- transport ---


def test_play_pause_from_the_shell_reaches_the_engine_both_ways(app, player) -> None:
    shell = player._shell
    adapter = player.controller.adapter

    shell.playPauseRequested.emit()
    app.processEvents()
    assert player.controller.state.status is PlaybackStatus.PAUSED
    assert adapter.pauses[-1] is True
    assert shell._playing is False

    shell.playPauseRequested.emit()
    app.processEvents()
    assert player.controller.state.status is PlaybackStatus.PLAYING
    assert adapter.pauses[-1] is False
    assert shell._playing is True


def test_a_seek_from_the_shell_lands_on_the_adapter_as_absolute_seconds(app, player) -> None:
    player._shell.seekRequested.emit(123.5)
    app.processEvents()
    assert player.controller.adapter.seeks[-1] == pytest.approx(123.5)
    assert player.controller.state.position == pytest.approx(123.5)


def test_space_still_reaches_the_shell_as_play_pause(app, player) -> None:
    _key(app, player._shell, Qt.Key.Key_Space)
    assert player.controller.state.status is PlaybackStatus.PAUSED
    _key(app, player._shell, Qt.Key.Key_Space)
    assert player.controller.state.status is PlaybackStatus.PLAYING


def test_the_dialog_forwards_a_key_instead_of_swallowing_it(app, player) -> None:
    _key(app, player, Qt.Key.Key_Space)
    assert player.controller.state.status is PlaybackStatus.PAUSED
    _key(app, player, Qt.Key.Key_M)
    assert player.controller.state.muted is True


# --- unit boundaries ---


def test_a_shell_volume_reaches_mpv_as_a_percentage_and_comes_back_as_a_fraction(
    app, player
) -> None:
    player._shell.volumeChanged.emit(0.4)
    app.processEvents()
    assert player.controller.adapter.volumes[-1] == 40
    assert player._shell._volume == pytest.approx(0.4)

    player.controller.adapter.volumeChanged.emit(75)
    app.processEvents()
    assert player._shell._volume == pytest.approx(0.75)


def test_a_rate_the_shell_can_pick_reaches_the_engine_unchanged(app, player) -> None:
    player._shell.rateChanged.emit(1.75)
    app.processEvents()
    assert player.controller.adapter.speeds[-1] == pytest.approx(1.75)


def test_a_rate_above_the_shell_ceiling_is_capped_for_the_shell_only(app, player) -> None:
    player.controller.adapter.speedChanged.emit(3.0)
    app.processEvents()
    assert player.controller.state.speed == pytest.approx(3.0)
    assert player._shell._rate == SHELL_RATE_MAX


# --- the quality list ---


def test_the_quality_popup_lists_every_height_and_picks_one(app, player) -> None:
    player.open_quality_menu()
    app.processEvents()
    assert [row.text() for row in _rows(player._quality_popup)] == ["1080p", "720p", "480p"]

    _rows(player._quality_popup)[1].click()
    app.processEvents()
    assert player.controller.adapter.loads[-1] == "u720"
    assert not player._quality_popup.isVisible()


# --- the episode drawer ---


def test_the_series_list_marks_the_current_episode_and_asks_for_the_pick(app, player) -> None:
    player.toggle_series()
    app.processEvents()
    rows = player._series.findChildren(QPushButton)
    assert [row.text() for row in rows] == [f"Серія {n}" for n in range(1, 6)]
    assert rows[2].objectName() == "playerSeriesItemCurrent"

    picked: list[tuple[dict, int]] = []
    player.episodeRequested.connect(lambda item, spot: picked.append((item, spot)))
    rows[3].click()
    app.processEvents()
    assert picked == [({"label": "Серія 4", "embed": "e4"}, 3)]
    assert not player._series.isVisible()


def test_the_episode_drawer_is_hidden_while_the_picture_is_fullscreen(app, player) -> None:
    player.toggle_series()
    assert player._series.isVisible()

    _key(app, player._shell, Qt.Key.Key_F)
    assert player._shell.state() == STATE_FULLSCREEN_ACTIVE
    assert not player._series.isVisible()

    _key(app, player._shell, Qt.Key.Key_F)
    assert not player._shell.state().startswith("FULLSCREEN")
    assert player._series.isVisible()


# --- the dub/sub popover ---


def test_the_track_popup_lists_every_track_marks_the_current_and_probes_the_rest(
    app, player
) -> None:
    probes: list[str] = []
    player.trackProbeRequested.connect(probes.append)

    player.open_track_menu()
    app.processEvents()
    rows = _rows(player._track_popup)
    assert [row.text() for row in rows] == ["Ashdi · Dub", "Ashdi · Sub"]
    assert rows[0].styleSheet() != rows[1].styleSheet()
    assert probes == ["sub"]

    player.set_track_quality("sub", 720)
    app.processEvents()
    assert _rows(player._track_popup)[1].text() == "Ashdi · Sub · 720p"


def test_picking_a_track_asks_the_title_page_for_it(app, player) -> None:
    requested: list[tuple[str, str]] = []
    player.variantRequested.connect(lambda embed, label: requested.append((embed, label)))

    player.open_track_menu()
    app.processEvents()
    _rows(player._track_popup)[1].click()
    app.processEvents()
    assert requested == [("sub", "Ashdi · Sub")]


# --- keyboard reach ---


def test_every_panel_opens_and_closes_from_the_keyboard(app, player) -> None:
    _key(app, player._shell, Qt.Key.Key_S)
    assert player._series.isVisible()
    _key(app, player._shell, Qt.Key.Key_Escape)
    assert not player._series.isVisible()
    assert player.isVisible(), "Esc closed a panel, not the player"

    _key(app, player._shell, Qt.Key.Key_T)
    assert player._track_popup.isVisible()
    requested: list[tuple[str, str]] = []
    player.variantRequested.connect(lambda embed, label: requested.append((embed, label)))
    # the highlight starts on the track already playing, so the second row is
    # one further press away
    _key(app, player._track_popup, Qt.Key.Key_Down)
    _key(app, player._track_popup, Qt.Key.Key_Down)
    _key(app, player._track_popup, Qt.Key.Key_Return)
    assert requested == [("sub", "Ashdi · Sub")]

    _key(app, player._shell, Qt.Key.Key_Q)
    assert player._quality_popup.isVisible()
    _key(app, player._quality_popup, Qt.Key.Key_End)
    _key(app, player._quality_popup, Qt.Key.Key_Return)
    assert player.controller.adapter.loads[-1] == "u480"
    assert not player._quality_popup.isVisible()
