from __future__ import annotations

from conftest import FakeMpvAdapter
from PySide6.QtCore import Qt
from PySide6.QtTest import QTest

from mikai_ui.player_controller import PlaybackState, PlaybackStatus, PlayerController
from mikai_ui.player_controls import PlayerControlsWidget
from mikai_ui.player_dialog import PlayerDialog


def ready_controller(adapter: FakeMpvAdapter, app) -> PlayerController:
    controller = PlayerController("stream-720", {1080: "stream-1080"}, 720)
    controller.attach_adapter(adapter)
    controller.load()
    adapter.durationChanged.emit(120)
    adapter.positionChanged.emit(25)
    adapter.mediaLoaded.emit()
    app.processEvents()
    return controller


def test_controller_owns_playback_transitions_and_state(app):
    adapter = FakeMpvAdapter()
    controller = PlayerController("stream")
    controller.attach_adapter(adapter)
    controller.load()
    assert controller.state.status is PlaybackStatus.LOADING
    adapter.mediaLoaded.emit()
    assert controller.state.status is PlaybackStatus.PLAYING

    controller.pause()
    assert controller.state.paused
    assert controller.state.status is PlaybackStatus.PAUSED
    adapter.bufferingChanged.emit(True)
    assert controller.state.status is PlaybackStatus.BUFFERING
    assert controller.state.paused
    adapter.bufferingChanged.emit(False)
    assert controller.state.status is PlaybackStatus.PAUSED

    controller.play()
    assert not controller.state.paused
    assert controller.state.status is PlaybackStatus.PLAYING
    adapter.mediaEnded.emit()
    assert controller.state.finished
    assert controller.state.paused
    adapter.errorOccurred.emit("codec details")
    assert controller.state.status is PlaybackStatus.ERROR
    assert controller.state.error == "codec details"
    controller.close()
    assert adapter.closed


def test_controller_seek_preview_commits_once_and_clamps(app):
    adapter = FakeMpvAdapter()
    controller = ready_controller(adapter, app)
    preview: list[float] = []
    controller.seekPreviewChanged.connect(preview.append)

    controller.begin_seek()
    controller.preview_seek(50)
    controller.preview_seek(70)
    assert preview == [50, 70]
    assert adapter.seeks == []
    controller.finish_seek(70)
    assert adapter.seeks == [70]
    assert controller.state.position == 70

    controller.rewind(1000)
    assert adapter.seeks[-1] == 0
    controller.forward(1000)
    assert adapter.seeks[-1] == 120
    controller.close()


def test_controller_volume_mute_speed_and_quality_resume(app):
    adapter = FakeMpvAdapter()
    controller = ready_controller(adapter, app)
    controller.set_volume(63)
    controller.toggle_mute()
    controller.set_speed(1.5)
    assert controller.state.volume == 63
    assert controller.state.muted
    assert controller.state.speed == 1.5
    assert adapter.volumes == [63]
    assert adapter.mutes == [True]
    assert adapter.speeds == [1.5]

    controller.pause()
    controller.set_quality(1080)
    assert adapter.loads[-1] == "stream-1080"
    assert controller.state.status is PlaybackStatus.LOADING
    adapter.mediaLoaded.emit()
    assert adapter.seeks[-1] == 25
    assert adapter.pauses[-1] is True
    assert adapter.volumes[-1] == 63
    assert adapter.mutes[-1] is True
    assert adapter.speeds[-1] == 1.5
    assert controller.state.status is PlaybackStatus.PAUSED
    assert controller.state.quality == 1080
    assert controller.state.position == 25
    controller.close()


def test_controls_render_state_and_timeline_without_engine_access(app):
    controls = PlayerControlsWidget()
    controls.set_playback_state(PlaybackState())
    assert not controls.play_button.isEnabled()

    controls.set_playback_state(
        PlaybackState(
            status=PlaybackStatus.PLAYING,
            position=30,
            duration=60,
            available=True,
        )
    )
    assert controls.play_button.isEnabled()
    assert controls.timeline.isEnabled()
    assert controls.timeline.value() == controls.timeline.SCALE // 2
    assert controls.time_label.text() == "0:30 / 1:00"

    controls.set_playback_state(
        PlaybackState(status=PlaybackStatus.ERROR, available=True, error="details")
    )
    assert not controls.play_button.isEnabled()
    assert not controls.timeline.isEnabled()
    controls.deleteLater()


class _StubMpv:
    """Minimal libmpv stand-in so the adapter's error policy can be exercised."""

    def __init__(self, reject: str | None = None) -> None:
        self.reject = reject
        self.seeks: list[tuple] = []
        self.played: list[str] = []

    def property_observer(self, _name):
        def decorate(fn):
            return fn

        return decorate

    def event_callback(self, _name):
        def decorate(fn):
            return fn

        return decorate

    def seek(self, amount, reference="relative", precision="keyframes"):
        if self.reject == "seek":
            raise RuntimeError("seek not supported on this stream")
        self.seeks.append((amount, reference, precision))

    def play(self, url):
        if self.reject == "play":
            raise RuntimeError("stream unreachable")
        self.played.append(url)


def _adapter(app, monkeypatch, stub):
    import sys
    import types

    from PySide6.QtWidgets import QWidget

    from mikai_ui.mpv_backend import MPVAdapter

    # A stand-in module keeps the test hermetic: the real one loads libmpv on import.
    fake_mpv = types.ModuleType("mpv")
    fake_mpv.MPV = lambda **_kwargs: stub
    monkeypatch.setitem(sys.modules, "mpv", fake_mpv)
    monkeypatch.setattr(MPVAdapter, "_resolve_runtime", staticmethod(lambda *a, **k: None))
    return MPVAdapter(QWidget())


def test_a_rejected_seek_keeps_playback_alive(app, monkeypatch):
    """Rewinding an HLS stream must not escalate a rejected seek to a fatal error."""
    stub = _StubMpv(reject="seek")
    adapter = _adapter(app, monkeypatch, stub)
    errors: list[str] = []
    adapter.errorOccurred.connect(errors.append)
    adapter.load("https://s.example/480/index.m3u8")
    assert stub.played == ["https://s.example/480/index.m3u8"]
    adapter.seek(120.0)
    assert errors == [], errors
    assert adapter._closed is False


def test_a_rejected_load_is_still_fatal(app, monkeypatch):
    """Only playback-critical commands escalate; a dead stream must still surface."""
    stub = _StubMpv(reject="play")
    adapter = _adapter(app, monkeypatch, stub)
    errors: list[str] = []
    adapter.errorOccurred.connect(errors.append)
    adapter.load("https://s.example/480/index.m3u8")
    assert errors, "a failed load must raise errorOccurred"


def test_seek_targets_an_absolute_time_on_keyframes(app, monkeypatch):
    """HLS segments start at keyframes, so exact (hr-seek) precision is the wrong ask."""
    stub = _StubMpv()
    adapter = _adapter(app, monkeypatch, stub)
    adapter.seek(90.0)
    assert stub.seeks == [(90.0, "absolute", "keyframes")]


def test_player_shortcuts_fullscreen_and_state_update(app):
    adapter = FakeMpvAdapter()
    dialog = PlayerDialog("Episode 1", "stream")
    dialog.controller.attach_adapter(adapter)
    dialog.controller.load()
    adapter.durationChanged.emit(80)
    adapter.positionChanged.emit(30)
    adapter.mediaLoaded.emit()
    dialog.show()
    app.processEvents()
    assert dialog.isFullScreen()
    assert dialog._time_label.text() == "0:30 / 1:20"

    QTest.keyClick(dialog, Qt.Key.Key_Space)
    assert adapter.pauses[-1] is True
    QTest.keyClick(dialog, Qt.Key.Key_Right)
    assert adapter.seeks[-1] == 40
    QTest.keyClick(dialog, Qt.Key.Key_M)
    assert dialog.controller.state.muted
    QTest.keyClick(dialog, Qt.Key.Key_Up)
    assert dialog.controller.state.volume == 100

    QTest.keyClick(dialog, Qt.Key.Key_F)
    app.processEvents()
    assert not dialog.isFullScreen()
    QTest.keyClick(dialog, Qt.Key.Key_Escape)
    assert not dialog.isFullScreen()
    QTest.mouseDClick(dialog._overlay, Qt.MouseButton.LeftButton)
    app.processEvents()
    assert dialog.isFullScreen()
    dialog.close()
    app.processEvents()
