from __future__ import annotations

from conftest import FakeMpvAdapter
from PySide6.QtCore import Qt
from PySide6.QtTest import QTest

from mikai_ui.player_controller import PlaybackStatus, PlayerController


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


def test_stylesheet_uses_only_properties_qt_understands(app):
    """Qt drops an entire stylesheet on one unknown property, so a typo like
    font-variant-numeric takes the whole design system down without a traceback.
    Qt reports it through C++ stderr, which pytest cannot intercept, so the sheet
    is checked against the documented property set instead."""
    import re

    from mikai_ui.theme import build_stylesheet

    supported = {
        "background",
        "border",
        "border-bottom",
        "border-color",
        "border-left",
        "border-bottom-left-radius",
        "border-bottom-right-radius",
        "border-radius",
        "border-top",
        "border-top-left-radius",
        "border-top-right-radius",
        "color",
        "font-family",
        "font-size",
        "font-weight",
        "height",
        "margin",
        "margin-right",
        "max-height",
        "max-width",
        "min-height",
        "min-width",
        "outline",
        "padding",
        "padding-top",
        "selection-background-color",
        "selection-color",
        "text-align",
        "top",
        "width",
    }
    used = set(re.findall(r"[{;]\s*([a-z-]+)\s*:", build_stylesheet()))
    assert not used - supported, sorted(used - supported)


def test_embedded_mpv_sends_the_headers_moonanime_requires():
    """MoonAnime's CDN answers 400 to a playlist that arrives without an
    Accept-Language header, so mpv, which sends none, is stuck on a stream that
    extraction reads fine. Measured: no Accept-Language = 400, present = 200."""
    from mikai_backend.streams import UA
    from mikai_ui.mpv_backend import PLAYER_LANGUAGE, embedded_mpv_options

    options = embedded_mpv_options("12345")
    assert options["user-agent"] == UA
    # mpv splits this list on commas, so the value must stay comma-free
    assert options["http-header-fields"] == f"Accept-Language: {PLAYER_LANGUAGE}"
    assert "," not in PLAYER_LANGUAGE


def test_switch_variant_resumes_at_the_same_position(app):
    """A track change must not rewind the episode: the position carries over."""
    adapter = FakeMpvAdapter()
    controller = PlayerController("stream-720", {720: "stream-720"}, 720)
    controller.attach_adapter(adapter)
    controller.load()
    adapter.durationChanged.emit(1200.0)
    adapter.positionChanged.emit(100.0)
    adapter.mediaLoaded.emit()
    app.processEvents()
    assert controller.state.position == 100.0

    controller.switch_variant("dub-1080", {1080: "dub-1080", 480: "dub-480"})
    assert adapter.loads[-1] == "dub-1080"
    app.processEvents()
    adapter.mediaLoaded.emit()
    app.processEvents()

    assert controller.state.position == 100.0, controller.state.position
    assert controller.state.quality == 1080
    assert adapter.seeks[-1] == 100.0, adapter.seeks
    controller.close()


def test_stream_rates_report_bitrate_and_throughput():
    """The second chip line carries what the video costs to pull: the track's own
    bitrate and the speed the app is actually downloading at."""
    from mikai_ui.player_controller import StreamInfo, format_rate

    assert format_rate(2_400_000) == "2.4 Mb/s"
    assert format_rate(850_000) == "850 kb/s"
    assert format_rate(0) == "0 kb/s"

    both = StreamInfo(1280, 720, 24.0, video_bitrate=2_400_000, cache_speed=3_150_000)
    assert both.rates() == "2.4 Mb/s · 3.1 Mb/s"
    # partial data shows what is known, never a zero
    assert StreamInfo(1280, 720, 24.0, video_bitrate=850_000).rates() == "850 kb/s"
    assert StreamInfo(1280, 720, 24.0).rates() == ""


def test_stream_info_text_uses_size_and_fps():
    from mikai_ui.player_controller import StreamInfo

    assert StreamInfo(1280, 720, 24.0).text() == "1280x720 | 24fps"
    assert StreamInfo(1920, 1080, 23.976).text() == "1920x1080 | 23.98fps"
    # mpv reports nothing useful until the stream is open; the chip stays empty
    assert StreamInfo().text() == ""
    assert StreamInfo(1280, 720, 0.0).text() == "1280x720"
    # container-fps is often 0 on HLS, so mpv's measured estimate stands in
    assert StreamInfo(1280, 720, 0.0, 23.976).text() == "1280x720 | 23.98fps"
    assert StreamInfo(1280, 720, 24.0, 30.0).text() == "1280x720 | 24fps"


def test_the_float_has_its_own_play_pause_and_no_caption(app):
    from mikai_ui.pip_window import PipWindow

    pip = PipWindow()
    assert pip.windowFlags() & Qt.WindowType.FramelessWindowHint
    # the bar carries play/pause and a close; there is no header
    toggled = []
    pip.togglePlayRequested.connect(lambda: toggled.append(True))
    pip._play_button.click()
    assert toggled == [True]
    pip.set_playing(False)
    assert "Продовжити" in pip._play_button.toolTip()
    pip.set_playing(True)
    assert "Пауза" in pip._play_button.toolTip()
    pip.close()


def test_the_float_seeks_with_the_arrow_keys(app):
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QKeyEvent

    from mikai_ui.pip_window import SEEK_STEP_SECONDS, PipWindow

    pip = PipWindow()
    seeks = []
    pip.seekRequested.connect(seeks.append)
    for key in (Qt.Key.Key_Left, Qt.Key.Key_Right):
        pip.keyPressEvent(
            QKeyEvent(
                QEvent.Type.KeyPress,
                key,
                Qt.KeyboardModifier.NoModifier,
                text="",
            )
        )
    assert seeks == [-SEEK_STEP_SECONDS, SEEK_STEP_SECONDS], seeks
    pip.close()


def test_the_float_toggles_on_a_click_and_moves_on_a_drag(app):
    """Telegram-style float: the picture is the big play/pause button, and a
    held drag moves the window (both in screen coordinates, so the drag is
    smooth no matter which child receives the event)."""
    from PySide6.QtCore import QPoint, Qt
    from PySide6.QtWidgets import QWidget

    from mikai_ui.pip_window import PipWindow

    class NativeVideo(QWidget):
        """Stands in for the mpv surface: a native child window swallows its
        mouse events instead of letting them propagate to the parent."""

        def mousePressEvent(self, event) -> None:  # noqa: N802 - Qt naming
            event.accept()

        def mouseMoveEvent(self, event) -> None:  # noqa: N802 - Qt naming
            event.accept()

        def mouseReleaseEvent(self, event) -> None:  # noqa: N802 - Qt naming
            event.accept()

    pip = PipWindow()
    pip.resize(480, 270)
    video = NativeVideo()
    pip.set_video(video)
    pip.move(QPoint(200, 150))
    pip.show()
    app.processEvents()
    toggled = []
    pip.togglePlayRequested.connect(lambda: toggled.append(True))
    overlay = pip._overlay

    QTest.mouseClick(overlay, Qt.MouseButton.LeftButton, pos=QPoint(100, 100))
    app.processEvents()
    assert toggled == [True], "a click on the picture is play/pause"

    before = pip.pos()
    QTest.mousePress(overlay, Qt.MouseButton.LeftButton, pos=QPoint(100, 100))
    for step in range(1, 12):
        QTest.mouseMove(overlay, QPoint(100 + step * 40, 100 + step * 10))
        app.processEvents()
    QTest.mouseRelease(overlay, Qt.MouseButton.LeftButton, pos=QPoint(540, 210))
    app.processEvents()
    assert pip.pos() != before, "a held drag moves the float"
    assert toggled == [True], "a drag is not a play/pause command"
    pip.close()
    app.processEvents()
