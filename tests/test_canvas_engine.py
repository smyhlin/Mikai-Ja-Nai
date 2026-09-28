from __future__ import annotations

from conftest import FakeMpvAdapter

from mikai_ui.player_controller import PlayerController


class _UnwritableCanvasAdapter(FakeMpvAdapter):
    """mpv is gone mid-gesture: the transform write is rejected."""

    def set_canvas(self, zoom: float, pan_x: float, pan_y: float) -> None:
        raise RuntimeError("mpv handle is gone")


def ready_controller(adapter: FakeMpvAdapter, app) -> PlayerController:
    controller = PlayerController("stream-720", {1080: "stream-1080"}, 720)
    controller.attach_adapter(adapter)
    controller.load()
    adapter.mediaLoaded.emit()
    app.processEvents()
    return controller


def test_canvas_transform_reaches_the_video_output(app):
    adapter = FakeMpvAdapter()
    controller = ready_controller(adapter, app)

    controller.apply_canvas(2.0, -0.25, 0.4)

    assert adapter.canvases == [(2.0, -0.25, 0.4)]
    controller.close()


def test_clearing_the_canvas_returns_the_picture_to_full_frame(app):
    adapter = FakeMpvAdapter()
    controller = ready_controller(adapter, app)
    controller.apply_canvas(3.5, 0.5, -0.5)

    controller.reset_canvas()

    assert adapter.canvases[-1] == (1.0, 0.0, 0.0)
    controller.close()


def test_a_transform_arriving_before_a_player_is_attached_is_dropped(app):
    controller = PlayerController("stream-720")

    controller.apply_canvas(2.0, 0.1, 0.2)
    controller.reset_canvas()

    adapter = FakeMpvAdapter()
    controller.attach_adapter(adapter)
    assert adapter.canvases == []
    controller.close()


def test_a_transform_arriving_after_the_controller_is_closed_is_dropped(app):
    adapter = FakeMpvAdapter()
    controller = ready_controller(adapter, app)
    controller.close()

    controller.apply_canvas(4.0, 0.3, 0.3)
    controller.reset_canvas()

    assert adapter.canvases == []


def test_pan_and_zoom_travel_unscaled_so_the_picture_matches_the_gesture(app):
    adapter = FakeMpvAdapter()
    controller = ready_controller(adapter, app)

    controller.apply_canvas(1.0, 0.0, 0.0)
    controller.apply_canvas(0.75, -1.5, 0.125)

    assert adapter.canvases == [(1.0, 0.0, 0.0), (0.75, -1.5, 0.125)]
    controller.close()


def test_a_rejected_transform_write_does_not_reach_the_caller(app):
    adapter = _UnwritableCanvasAdapter()
    controller = ready_controller(adapter, app)

    controller.apply_canvas(2.0, 0.1, -0.1)
    controller.reset_canvas()

    assert controller.state.error is None
    assert controller.state.playing

    controller.pause()
    assert controller.state.paused
    controller.play()
    assert controller.state.playing
    controller.close()
