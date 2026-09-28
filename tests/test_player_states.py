"""The player driven through its display states: wheel, double click, drag,
fullscreen idle, and the chrome each state shows.
"""

from __future__ import annotations

import pytest
from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QWheelEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QPushButton

from conftest import FakeMpvAdapter

from mikai_ui.player_dialog import PlayerDialog
from mikai_ui.player_view import ZOOM_MAGIFIED, ZOOM_MIN, ViewState


def _wheel(widget, dy: int) -> None:
    at = QPointF(widget.width() / 2, widget.height() / 2)
    QApplication.sendEvent(
        widget,
        QWheelEvent(
            at,
            at,
            QPoint(),
            QPoint(0, dy),
            Qt.MouseButton.NoButton,
            Qt.KeyboardModifier.NoModifier,
            Qt.ScrollPhase.NoScrollPhase,
            False,
        ),
    )


@pytest.fixture
def player(app):
    dialog = PlayerDialog(
        "Серія 7",
        "https://s.example/720.m3u8",
        qualities={1080: "u1080", 720: "u720"},
        initial_height=720,
        playlist=[{"label": f"Серія {n}", "embed": f"e{n}"} for n in range(1, 6)],
        episode_index=2,
    )
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


# --- canvas ---


def test_the_wheel_over_the_picture_zooms_it(app, player) -> None:
    _wheel(player._video, 120)
    assert player._view.scale > 1.0
    assert player._view.state is ViewState.ZOOM_IN_PAN


def test_the_zoom_reaches_the_video_engine(app, player) -> None:
    adapter = FakeMpvAdapter()
    player.controller.attach_adapter(adapter)
    _wheel(player._video, 120)
    app.processEvents()
    assert adapter.canvases, "no canvas transform was sent to the engine"
    zoom, _, _ = adapter.canvases[-1]
    assert zoom == pytest.approx(player._view.scale)


def test_zooming_out_stops_at_the_smallest_scale(app, player) -> None:
    for _ in range(40):
        _wheel(player._video, -120)
    assert player._view.scale == ZOOM_MIN
    assert player._view.state is ViewState.SCROLL_OUT


def test_a_second_double_click_returns_the_picture_to_the_fit(app, player) -> None:
    QTest.mouseDClick(player._video, Qt.MouseButton.LeftButton)
    app.processEvents()
    assert player._view.scale == ZOOM_MAGIFIED
    QTest.mouseDClick(player._video, Qt.MouseButton.LeftButton)
    app.processEvents()
    assert player._view.scale == pytest.approx(1.0)


def test_dragging_a_magnified_picture_moves_it_and_does_not_toggle_playback(app, player) -> None:
    toggled: list[bool] = []
    player._controller.toggle_play_pause = lambda: toggled.append(True)
    player._view.set_scale(3.0)
    centre = QPoint(player._video.width() // 2, player._video.height() // 2)
    QTest.mousePress(player._video, Qt.MouseButton.LeftButton, pos=centre)
    QTest.mouseMove(player._video, centre + QPoint(120, 0))
    app.processEvents()
    QTest.mouseRelease(player._video, Qt.MouseButton.LeftButton, pos=centre + QPoint(120, 0))
    app.processEvents()
    assert player._view.pan[0] > 0, player._view.pan
    assert toggled == []


def test_a_nudged_click_does_not_drag_the_picture(app, player) -> None:
    player._view.set_scale(3.0)
    centre = QPoint(player._video.width() // 2, player._video.height() // 2)
    QTest.mousePress(player._video, Qt.MouseButton.LeftButton, pos=centre)
    QTest.mouseMove(player._video, centre + QPoint(1, 1))
    app.processEvents()
    QTest.mouseRelease(player._video, Qt.MouseButton.LeftButton, pos=centre + QPoint(1, 1))
    app.processEvents()
    assert player._view.pan == (0.0, 0.0)


def test_holding_the_picture_turns_the_grab_cursor_into_a_closed_hand(app, player) -> None:
    player._fullscreen.exit()
    player._view.set_scale(3.0)
    app.processEvents()
    centre = QPoint(player._video.width() // 2, player._video.height() // 2)
    QTest.mousePress(player._video, Qt.MouseButton.LeftButton, pos=centre)
    QTest.mouseMove(player._video, centre + QPoint(40, 0))
    app.processEvents()
    assert player._video.cursor().shape() == Qt.CursorShape.ClosedHandCursor
    QTest.mouseRelease(player._video, Qt.MouseButton.LeftButton, pos=centre + QPoint(40, 0))
    app.processEvents()
    assert player._video.cursor().shape() == Qt.CursorShape.OpenHandCursor


def test_a_dragged_picture_stops_at_its_edge(app, player) -> None:
    player._view.set_scale(2.0)
    limit = (2.0 - 1.0) / 2
    QTest.mousePress(player._video, Qt.MouseButton.LeftButton, pos=QPoint(10, 10))
    for step in range(1, 30):
        QTest.mouseMove(player._video, QPoint(10 + step * 40, 10))
        app.processEvents()
    QTest.mouseRelease(player._video, Qt.MouseButton.LeftButton, pos=QPoint(1200, 10))
    app.processEvents()
    assert player._view.pan[0] == pytest.approx(limit), player._view.pan


# --- chrome per state ---


def test_the_fitted_state_shows_the_gallery_chevrons(app, player) -> None:
    player._fullscreen.exit()
    player._view.reset_canvas()
    app.processEvents()
    assert player._chevrons.isVisible()
    assert player._footer.isVisible()


def test_magnifying_hides_the_chevrons_and_lifts_the_toolbar(app, player) -> None:
    player._fullscreen.exit()
    app.processEvents()
    player._view.set_scale(3.0)
    app.processEvents()
    assert not player._chevrons.isVisible()
    # a native handle is what lets the dock sit over the picture without
    # hiding it; docked again, it goes back to being a plain layout child
    assert player._controls.testAttribute(Qt.WidgetAttribute.WA_NativeWindow)
    player._view.reset_canvas()
    app.processEvents()
    assert player._chevrons.isVisible()
    assert player._controls_row.indexOf(player._controls) >= 0


def test_fullscreen_drops_the_footer_and_the_source_pill(app, player) -> None:
    assert player._view.state is ViewState.FULLSCREEN_ACTIVE
    assert not player._footer.isVisible()
    assert not player._source_pill.isVisible()
    assert player._window_controls.isVisible()


def test_the_fullscreen_idle_hides_everything_and_a_mouse_wakes_it(app, player) -> None:
    assert player._overlay._hide_timer.interval() == 2500
    player._view.hide_chrome()
    app.processEvents()
    assert player._view.state is ViewState.FULLSCREEN_AUTO_HIDDEN
    assert not player._window_controls.isVisible()
    assert player._video.cursor().shape() == Qt.CursorShape.BlankCursor
    player._view.wake()
    app.processEvents()
    assert player._view.state is ViewState.FULLSCREEN_ACTIVE
    assert player._window_controls.isVisible()


def test_the_picture_carries_a_grab_cursor_only_while_magnified(app, player) -> None:
    player._fullscreen.exit()
    player._view.reset_canvas()
    app.processEvents()
    assert player._video.cursor().shape() == Qt.CursorShape.ArrowCursor
    player._view.set_scale(3.0)
    app.processEvents()
    assert player._video.cursor().shape() == Qt.CursorShape.OpenHandCursor


# --- content ---


def test_the_source_pill_names_the_track_the_picture_came_from(app) -> None:
    dialog = PlayerDialog(
        "Серія 7",
        "https://s.example/720.m3u8",
        qualities={1080: "u1080", 720: "u720"},
        initial_height=720,
        variants=[
            {"embed": "dub1", "display": "Озвучення", "track": "Ashdi", "quality": 1080},
            {"embed": "sub1", "display": "Субтитри", "track": "SunDragon", "quality": 720},
        ],
        current_embed="sub1",
    )
    dialog.show()
    app.processEvents()
    assert dialog._source_pill.link_button.text() == "Субтитри · SunDragon"
    assert dialog._source_pill.detail_label.text() == "720p"
    dialog.close()
    app.processEvents()


def test_the_source_pill_hangs_off_the_picture_not_the_dock(app, player) -> None:
    """Anchoring the pill to the stage dropped it straight onto the control bar."""
    player._fullscreen.exit()
    app.processEvents()
    frame = player._video.geometry()
    pill = player._source_pill.geometry()
    assert frame.top() <= pill.top()
    assert pill.bottom() <= frame.bottom(), (pill, frame)
    assert not pill.intersects(player._controls.geometry()), (pill, player._controls.geometry())
    assert not pill.intersects(player._footer.geometry()), (pill, player._footer.geometry())


def test_the_footer_shows_the_episode_and_offers_to_reach_the_stream(app) -> None:
    dialog = PlayerDialog("Серія 12", "https://s.example/1080.m3u8")
    dialog.show()
    app.processEvents()
    dialog._fullscreen.exit()
    app.processEvents()
    assert dialog._footer.name_label.text() == "Серія 12"
    assert dialog._footer.open_button.accessibleName()
    dialog.close()
    app.processEvents()


def test_the_speed_flyout_offers_the_named_presets(app) -> None:
    dialog = PlayerDialog("Серія 1", "stream")
    dialog.show()
    app.processEvents()
    flyout = dialog._controls.speed_flyout
    labels = [b.text() for b in flyout.findChildren(QPushButton)]
    assert "1.0 Звичайна" in labels
    assert "2.0 Подвійна" in labels
    picked: list[float] = []
    flyout.speedChanged.connect(picked.append)
    flyout.findChildren(QPushButton)[labels.index("1.5 Швидка")].click()
    app.processEvents()
    assert picked == [1.5]
    dialog.close()
    app.processEvents()


# --- window controls, chevrons, footer actions ---


def test_the_only_window_control_closes_the_player(app) -> None:
    """The frame is gone: one dismiss control, like the cover viewer.

    There is deliberately no minimise and no expand. Minimise from fullscreen
    left the window simultaneously minimised and fullscreen, and expand
    duplicated F and the dock's own fullscreen control.
    """
    dialog = PlayerDialog("Серія 1", "stream")
    dialog.show()
    app.processEvents()
    assert dialog._fullscreen.is_fullscreen

    names = [b.objectName() for b in dialog._window_controls.findChildren(QPushButton)]
    assert names == ["playerDismissButton"], names

    dialog._window_controls.close_button.click()
    app.processEvents()
    assert not dialog.isVisible()
    dialog.close()


def test_escape_unwinds_the_float_then_fullscreen_then_the_player(app) -> None:
    dialog = PlayerDialog("Серія 1", "stream")
    dialog.show()
    app.processEvents()
    assert dialog._fullscreen.is_fullscreen

    QTest.keyClick(dialog, Qt.Key.Key_Escape)
    app.processEvents()
    assert not dialog._fullscreen.is_fullscreen
    assert dialog.isVisible(), "the first escape leaves fullscreen, it does not close"

    QTest.keyClick(dialog, Qt.Key.Key_Escape)
    app.processEvents()
    assert not dialog.isVisible()
    dialog.close()


def test_the_chevrons_step_through_the_playlist(app, player) -> None:
    player._fullscreen.exit()
    app.processEvents()
    # the fixture sits on episode 3 of 5
    assert player._chevrons.previous_button.isEnabled()
    assert player._chevrons.next_button.isEnabled()

    asked: list[int] = []
    player.episodeRequested.connect(lambda entry, index: asked.append(index))
    player._chevrons.next_button.click()
    player._chevrons.previous_button.click()
    app.processEvents()
    # the dialog only asks; the title page confirms by calling apply_episode,
    # so both requests are measured from the same current episode
    assert asked == [3, 1], asked

    player.apply_episode("Серія 4", "e4", 3, {720: "u720"}, player._variants)
    app.processEvents()
    player._chevrons.next_button.click()
    app.processEvents()
    assert asked == [3, 1, 4], asked


def test_the_chevrons_go_quiet_at_the_ends_of_the_playlist(app) -> None:
    dialog = PlayerDialog(
        "Серія 1",
        "stream",
        playlist=[{"label": "Серія 1", "embed": "e1"}],
        episode_index=0,
    )
    dialog.show()
    app.processEvents()
    dialog._fullscreen.exit()
    app.processEvents()
    assert not dialog._chevrons.previous_button.isEnabled()
    assert not dialog._chevrons.next_button.isEnabled()
    dialog.close()


def test_the_footer_and_the_pill_both_reach_the_stream(app) -> None:
    import mikai_ui.player_dialog as module

    opened: list[str] = []
    original = module.QDesktopServices.openUrl
    module.QDesktopServices.openUrl = lambda url: opened.append(url.toString())
    try:
        dialog = PlayerDialog("Серія 1", "https://s.example/720.m3u8")
        dialog.show()
        app.processEvents()
        dialog._footer.open_button.click()
        dialog._source_pill.link_button.click()
        app.processEvents()
        assert opened == [
            "https://s.example/720.m3u8",
            "https://s.example/720.m3u8",
        ], opened
        dialog.close()
    finally:
        module.QDesktopServices.openUrl = original


def test_the_footer_copies_the_stream_url(app) -> None:
    from PySide6.QtGui import QGuiApplication

    dialog = PlayerDialog("Серія 1", "https://s.example/720.m3u8")
    dialog.show()
    app.processEvents()
    dialog._footer.more_button.click()
    app.processEvents()
    assert QGuiApplication.clipboard().text() == "https://s.example/720.m3u8"
    dialog.close()
