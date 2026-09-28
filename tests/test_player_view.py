"""The view state machine: transitions, clamping, and the chrome matrix."""

from __future__ import annotations

import pytest

from mikai_ui.player_view import (
    PAN_THRESHOLD_PX,
    ZOOM_FIT,
    ZOOM_MAGIFIED,
    ZOOM_MAX,
    ZOOM_MIN,
    ZOOM_STEP,
    Chrome,
    Cursor,
    ViewMachine,
    ViewState,
    chrome_for,
    clamp_pan,
    clamp_scale,
    pan_limit,
)


@pytest.fixture
def machine() -> ViewMachine:
    return ViewMachine()


def test_a_fresh_player_is_fitted_with_everything_visible(machine: ViewMachine) -> None:
    assert machine.state is ViewState.DEFAULT_FIT
    assert machine.chrome == Chrome(True, True, True, True, True, Cursor.DEFAULT, False)


def test_zooming_out_beyond_fit_enters_the_scroll_out_state(machine: ViewMachine) -> None:
    machine.zoom_by(-ZOOM_STEP)
    assert machine.state is ViewState.SCROLL_OUT
    machine.zoom_by(ZOOM_STEP)
    assert machine.state is ViewState.DEFAULT_FIT


def test_zooming_in_past_fit_enables_panning_and_hides_the_gallery_chevrons(
    machine: ViewMachine,
) -> None:
    machine.zoom_by(ZOOM_STEP)
    assert machine.state is ViewState.ZOOM_IN_PAN
    assert machine.can_pan
    assert machine.chrome.gallery_navigation is False
    assert machine.chrome.window_controls is True


def test_the_magnified_state_lifts_the_toolbar_over_the_picture(machine: ViewMachine) -> None:
    machine.toggle_magnify()
    assert machine.scale == ZOOM_MAGIFIED
    assert machine.chrome.floating_toolbar is True
    assert machine.chrome.cursor is Cursor.GRAB


def test_scale_is_clamped_to_the_gesture_limits(machine: ViewMachine) -> None:
    machine.zoom_by(1000)
    assert machine.scale == ZOOM_MAX
    machine.zoom_by(-1000)
    assert machine.scale == ZOOM_MIN
    assert clamp_scale(-5) == ZOOM_MIN
    assert clamp_scale(99) == ZOOM_MAX


def test_pan_stops_at_the_picture_edge_and_cannot_escape_it(machine: ViewMachine) -> None:
    machine.set_scale(3.0)
    machine.pan_by(99, 99)
    assert machine.pan == (pan_limit(3.0), pan_limit(3.0))
    machine.pan_by(-99, -99)
    assert machine.pan == (-pan_limit(3.0), -pan_limit(3.0))


def test_a_shrunk_picture_cannot_be_dragged_off_centre(machine: ViewMachine) -> None:
    """Below the fit scale the video is smaller than the frame; panning would
    only float it in black."""
    machine.set_scale(0.5)
    assert pan_limit(0.5) == 0.0
    machine.pan_by(5, 5)
    assert machine.pan == (0.0, 0.0)


def test_pan_is_ignored_outside_the_magnified_state(machine: ViewMachine) -> None:
    machine.pan_by(3, 3)
    assert machine.pan == (0.0, 0.0)


def test_zooming_back_to_fit_pulls_a_dragged_picture_recentres_it(
    machine: ViewMachine,
) -> None:
    machine.set_scale(4.0)
    machine.pan_by(2, 2)
    assert machine.pan != (0.0, 0.0)
    machine.set_scale(ZOOM_FIT)
    assert machine.pan == (0.0, 0.0)


def test_clamp_pan_bounds_a_value_to_the_scale(machine: ViewMachine) -> None:
    assert clamp_pan(5, 2.0) == pan_limit(2.0)
    assert clamp_pan(-5, 2.0) == -pan_limit(2.0)
    assert clamp_pan(5, 1.0) == 0.0


def test_fullscreen_idle_hides_the_chrome_and_waking_restores_it(machine: ViewMachine) -> None:
    machine.enter_fullscreen()
    assert machine.state is ViewState.FULLSCREEN_ACTIVE
    machine.hide_chrome()
    assert machine.state is ViewState.FULLSCREEN_AUTO_HIDDEN
    assert machine.chrome.cursor is Cursor.NONE
    assert machine.chrome.playback_toolbar is False
    assert machine.chrome.window_controls is False
    machine.wake()
    assert machine.state is ViewState.FULLSCREEN_ACTIVE
    assert machine.chrome.cursor is Cursor.DEFAULT


def test_fullscreen_keeps_the_toolbar_docked_and_drops_the_footer(
    machine: ViewMachine,
) -> None:
    machine.enter_fullscreen()
    assert machine.chrome.playback_toolbar is True
    assert machine.chrome.floating_toolbar is False
    assert machine.chrome.metadata_footer is False
    assert machine.chrome.source_badge is False


def test_leaving_fullscreen_never_returns_to_the_hidden_chrome(machine: ViewMachine) -> None:
    machine.enter_fullscreen()
    machine.hide_chrome()
    machine.exit_fullscreen()
    assert machine.state is ViewState.DEFAULT_FIT
    assert machine.chrome.playback_toolbar is True


def test_a_zoomed_picture_keeps_its_scale_across_a_fullscreen_round_trip(
    machine: ViewMachine,
) -> None:
    """The zoom belongs to the media, not to the window mode."""
    machine.set_scale(3.0)
    machine.enter_fullscreen()
    machine.exit_fullscreen()
    assert machine.state is ViewState.ZOOM_IN_PAN
    assert machine.scale == 3.0


def test_magnifying_inside_fullscreen_reports_the_magnified_state(machine: ViewMachine) -> None:
    """The player opens in fullscreen, so a state that fullscreen owned would
    make the wheel and the double click dead on arrival."""
    machine.enter_fullscreen()
    assert machine.state is ViewState.FULLSCREEN_ACTIVE
    machine.set_scale(2.2)
    assert machine.state is ViewState.ZOOM_IN_PAN
    assert machine.fullscreen is True


def test_the_idle_hide_only_bites_once_the_picture_is_back_to_the_fit(
    machine: ViewMachine,
) -> None:
    machine.enter_fullscreen()
    machine.set_scale(2.2)
    machine.hide_chrome()
    assert machine.state is ViewState.ZOOM_IN_PAN
    machine.set_scale(1.0)
    assert machine.state is ViewState.FULLSCREEN_AUTO_HIDDEN


def test_hiding_the_chrome_outside_fullscreen_does_nothing(machine: ViewMachine) -> None:
    machine.hide_chrome()
    assert machine.state is ViewState.DEFAULT_FIT


def test_listeners_see_every_change(machine: ViewMachine) -> None:
    seen: list[ViewState] = []
    machine.subscribe(lambda m: seen.append(m.state))
    machine.zoom_by(ZOOM_STEP)
    machine.reset_canvas()
    machine.enter_fullscreen()
    machine.hide_chrome()
    assert seen == [
        ViewState.ZOOM_IN_PAN,
        ViewState.DEFAULT_FIT,
        ViewState.FULLSCREEN_ACTIVE,
        ViewState.FULLSCREEN_AUTO_HIDDEN,
    ]


def test_reset_returns_the_canvas_to_the_fit(machine: ViewMachine) -> None:
    machine.set_scale(4.5)
    machine.pan_by(1, 1)
    machine.reset_canvas()
    assert machine.scale == ZOOM_FIT
    assert machine.pan == (0.0, 0.0)
    assert machine.state is ViewState.DEFAULT_FIT


def test_every_state_declares_its_own_chrome() -> None:
    for state in ViewState:
        assert chrome_for(state).cursor is not None
    assert chrome_for(ViewState.SCROLL_OUT).cursor is Cursor.ZOOM


def test_the_drag_threshold_matches_the_spec() -> None:
    assert PAN_THRESHOLD_PX == 3
