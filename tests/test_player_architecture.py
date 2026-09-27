from __future__ import annotations

from conftest import FakeMpvAdapter
from PySide6.QtCore import Qt
from PySide6.QtTest import QTest

from mikai_ui.player_controller import PlaybackState, PlaybackStatus, PlayerController
from mikai_ui.player_controls import PlayerControlsWidget
from mikai_ui.player_dialog import PlayerDialog
from mikai_ui.theme.metrics import TIMELINE_ROW_HEIGHT


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


def test_clicking_the_timeline_seeks_to_that_point(app):
    """QSlider only emits its seek signals when its own handle is grabbed, so a
    click on the track body used to do nothing at all."""
    from PySide6.QtCore import QPoint
    from PySide6.QtTest import QTest

    from mikai_ui.player_controls import TimelineSlider

    timeline = TimelineSlider()
    timeline.resize(400, TIMELINE_ROW_HEIGHT)
    timeline.set_playback_position(0.0, 200.0)
    timeline.show()
    app.processEvents()

    finished: list[float] = []
    previews: list[float] = []
    started: list[bool] = []
    timeline.seekStarted.connect(lambda: started.append(True))
    timeline.seekPreviewChanged.connect(previews.append)
    timeline.seekFinished.connect(finished.append)

    QTest.mouseClick(
        timeline,
        Qt.MouseButton.LeftButton,
        pos=QPoint(int(timeline.width() * 0.75), timeline.height() // 2),
    )
    app.processEvents()

    assert started, "a click on the track must begin a seek"
    # 75% of the track is 75% of the video, not half a handle off
    assert finished == [150.0], finished
    assert previews[-1] == 150.0
    timeline.deleteLater()


def test_dragging_the_timeline_previews_each_step_and_commits_once(app):
    from PySide6.QtCore import QPoint
    from PySide6.QtTest import QTest

    from mikai_ui.player_controls import TimelineSlider

    timeline = TimelineSlider()
    timeline.resize(400, TIMELINE_ROW_HEIGHT)
    timeline.set_playback_position(0.0, 200.0)
    timeline.show()
    app.processEvents()

    previews: list[float] = []
    finished: list[float] = []
    timeline.seekPreviewChanged.connect(previews.append)
    timeline.seekFinished.connect(finished.append)

    height = timeline.height() // 2
    QTest.mousePress(
        timeline, Qt.MouseButton.LeftButton, pos=QPoint(int(timeline.width() * 0.25), height)
    )
    QTest.mouseMove(timeline, QPoint(int(timeline.width() * 0.10), height))
    QTest.mouseRelease(
        timeline, Qt.MouseButton.LeftButton, pos=QPoint(int(timeline.width() * 0.10), height)
    )
    app.processEvents()

    assert previews == [50.0, 20.0], previews
    assert finished == [20.0], finished
    timeline.deleteLater()


def test_scrubbing_the_player_bar_reaches_the_engine_once(app):
    """End to end: one click on the real control bar is one seek command."""
    from PySide6.QtCore import QPoint
    from PySide6.QtTest import QTest

    from mikai_ui.player_dialog import PlayerDialog

    dialog = PlayerDialog("Серія 1", "https://s.example/480/index.m3u8")
    adapter = FakeMpvAdapter()
    dialog.controller.attach_adapter(adapter)
    dialog.controller.load()
    adapter.durationChanged.emit(200.0)
    adapter.mediaLoaded.emit()
    app.processEvents()

    timeline = dialog._timeline
    QTest.mouseClick(
        timeline,
        Qt.MouseButton.LeftButton,
        pos=QPoint(int(timeline.width() * 0.5), timeline.height() // 2),
    )
    app.processEvents()

    assert adapter.seeks == [100.0], adapter.seeks
    dialog.close()
    app.processEvents()


def test_picking_a_row_switches_quality_and_adopting_one_does_not(app):
    """A row click is a user action and must reach the engine; a height the stream
    chose for itself must not bounce back as a user action."""
    from PySide6.QtWidgets import QPushButton

    from mikai_ui.player_controls import PlayerControlsWidget

    controls = PlayerControlsWidget()
    controls.resize(900, 120)
    controls.set_qualities({1080: "u", 720: "u", 480: "u"}, 1080)
    controls.show()
    app.processEvents()
    picker = controls.quality_picker

    picked = []
    controls.qualityChanged.connect(picked.append)

    picker._present()
    app.processEvents()
    rows = [
        w
        for w in picker._panel.findChildren(QPushButton)
        if w.objectName().startswith("qualityRow")
    ]
    rows[2].click()
    app.processEvents()
    assert picked == [480], picked
    assert picker.text().startswith("480p")
    assert picker._panel is None, "the panel closes on pick"

    picker.adopt_external(720)
    assert picked == [480], "an external height must not re-emit"
    assert picker.text().startswith("720p")
    controls.deleteLater()


def test_quality_selector_panel_is_readable_and_never_scrolls(app):
    """A QComboBox popup is a scroll view that can hide entries; the picker sizes
    to its rows instead, so every quality is visible at once."""
    from PySide6.QtWidgets import QPushButton, QScrollArea

    from mikai_ui.player_controls import PlayerControlsWidget

    controls = PlayerControlsWidget()
    controls.resize(900, 120)
    controls.set_qualities({2160: "u", 1080: "u", 720: "u", 480: "u", 360: "u"}, 1080)
    controls.show()
    app.processEvents()

    picker = controls.quality_picker
    assert picker.height() >= 30, picker.height()
    assert "Full HD" in picker.text(), picker.text()

    picker._present()
    app.processEvents()
    panel = picker._panel
    rows = [w for w in panel.findChildren(QPushButton) if w.objectName().startswith("qualityRow")]
    assert len(rows) == 5, [r.text() for r in rows]
    assert all(row.height() >= 30 for row in rows)
    assert panel.findChild(QScrollArea) is None, "the panel must not scroll"
    assert [r.objectName() for r in rows].count("qualityRowCurrent") == 1
    assert rows[1].objectName() == "qualityRowCurrent"
    picker._dismiss()
    controls.deleteLater()


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


def test_episode_stepping_moves_through_the_playlist_and_stops_at_the_ends(app):
    from mikai_ui.player_dialog import PlayerDialog

    playlist = [{"label": f"Серія {n}", "embed": f"https://s/{n}"} for n in range(1, 4)]
    dialog = PlayerDialog("Серія 1", "https://s/720.m3u8", playlist=playlist, episode_index=0)
    dialog.show()
    app.processEvents()

    controls = dialog._controls
    # no previous episode at the head of the list
    assert not controls.prev_ep_button.isVisible()
    assert controls.next_ep_button.isVisible()

    asked = []
    dialog.episodeRequested.connect(lambda item, spot: asked.append((item["label"], spot)))
    dialog.step_episode(1)
    app.processEvents()
    assert asked == [("Серія 2", 1)], asked

    # walking past the end asks for nothing rather than wrapping around
    dialog._episode_index = 2
    dialog.step_episode(1)
    app.processEvents()
    assert asked == [("Серія 2", 1)], asked
    dialog.close()
    app.processEvents()


def test_episode_sidebar_collapses_to_a_strip_and_stays_clear_of_the_video(app):
    from PySide6.QtWidgets import QPushButton

    from mikai_ui.player_dialog import PlayerDialog
    from mikai_ui.theme.metrics import PLAYER_SIDEBAR_STRIP, PLAYER_SIDEBAR_WIDTH

    playlist = [{"label": f"Серія {n}", "embed": f"https://s/{n}"} for n in range(1, 4)]
    dialog = PlayerDialog("Серія 2", "https://s/720.m3u8", playlist=playlist, episode_index=1)
    dialog.resize(1000, 560)
    dialog.show()
    app.processEvents()

    sidebar = dialog._sidebar
    assert sidebar.width() == PLAYER_SIDEBAR_STRIP
    assert not dialog._sidebar_scroll.isVisible()

    dialog.toggle_sidebar()
    app.processEvents()
    assert dialog._sidebar_scroll.isVisible()
    assert sidebar.width() == PLAYER_SIDEBAR_STRIP + PLAYER_SIDEBAR_WIDTH

    rows = [
        b
        for b in dialog._sidebar_list.findChildren(QPushButton)
        if b.objectName().startswith("playerEpisodeItem")
    ]
    assert [r.text() for r in rows] == ["Серія 1", "Серія 2", "Серія 3"]
    assert rows[1].objectName() == "playerEpisodeItemCurrent"

    # the sidebar is a sibling of the video, so expanding narrows the video
    # rather than covering the picture
    assert dialog._video.width() + PLAYER_SIDEBAR_STRIP <= dialog._stage.width()
    assert not dialog._video.geometry().intersects(sidebar.geometry())

    asked = []
    dialog.episodeRequested.connect(lambda item, spot: asked.append((item["label"], spot)))
    rows[0].click()
    app.processEvents()
    assert asked == [("Серія 1", 0)], asked
    dialog.close()
    app.processEvents()


def test_the_collapsed_rail_is_entirely_clickable(app):
    """The toggle used to be a 28px square at the top, so a click anywhere else
    on the rail did nothing."""
    from PySide6.QtCore import QPoint, Qt
    from PySide6.QtTest import QTest

    from mikai_ui.player_dialog import PlayerDialog

    dialog = PlayerDialog(
        "Серія 2",
        "https://s/720.m3u8",
        playlist=[{"label": f"Серія {n}", "embed": f"https://s/{n}"} for n in range(1, 5)],
        episode_index=1,
    )
    dialog.resize(900, 520)
    dialog.show()
    app.processEvents()

    rail = dialog._sidebar_toggle
    assert rail.height() == dialog._sidebar.height(), (rail.height(), dialog._sidebar.height())

    # a click near the bottom of the rail, well clear of the chevron
    QTest.mouseClick(
        rail, Qt.MouseButton.LeftButton, pos=QPoint(rail.width() // 2, rail.height() - 8)
    )
    app.processEvents()
    assert dialog._sidebar_scroll.isVisible(), "the whole rail must be clickable"

    # and it collapses back to a full-height rail
    QTest.mouseClick(
        rail, Qt.MouseButton.LeftButton, pos=QPoint(rail.width() // 2, rail.height() - 8)
    )
    app.processEvents()
    assert not dialog._sidebar_scroll.isVisible()
    assert rail.height() == dialog._sidebar.height()
    dialog.close()
    app.processEvents()


def test_the_episode_rail_hides_with_the_chrome(app):
    """Idle must clear every element, the rail included: a stripe of UI over a
    playing video is worse than no sidebar."""
    from PySide6.QtTest import QTest

    from mikai_ui.player_dialog import PlayerDialog

    dialog = PlayerDialog(
        "Серія 2",
        "https://s/720.m3u8",
        playlist=[{"label": f"Серія {n}", "embed": f"https://s/{n}"} for n in range(1, 5)],
        episode_index=1,
    )
    dialog.resize(900, 520)
    dialog.show()
    app.processEvents()
    dialog._overlay.set_fullscreen(True)
    dialog._overlay.show_controls()
    app.processEvents()
    assert dialog._sidebar.isVisible() and dialog._controls.isVisible()

    dialog._overlay.hide_controls()
    QTest.qWait(500)
    app.processEvents()
    assert not dialog._sidebar.isVisible(), "the rail must not outlive the bar"
    assert not dialog._controls.isVisible()
    # the video takes the full width back
    assert dialog._video.width() == dialog._stage.width()

    dialog._overlay.show_controls()
    app.processEvents()
    assert dialog._sidebar.isVisible(), "the rail returns with the chrome"
    dialog.close()
    app.processEvents()


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


def test_stream_chip_shows_on_mouse_movement_only(app):
    from mikai_ui.player_controller import PlaybackState, PlaybackStatus, StreamInfo
    from mikai_ui.player_dialog import PlayerDialog

    dialog = PlayerDialog("Серія 1", "https://s.example/480/index.m3u8")
    dialog.show()
    app.processEvents()
    chip = dialog._overlay.stream_chip
    assert not chip.isVisible()

    dialog._overlay.set_playback_state(
        PlaybackState(
            status=PlaybackStatus.PLAYING, available=True, stream=StreamInfo(1280, 720, 24.0)
        )
    )
    app.processEvents()
    assert dialog._overlay.stream_line.text() == "1280x720 | 24fps"
    assert not chip.isVisible(), "the chip must stay hidden until the mouse moves"

    dialog._overlay.show_controls()
    app.processEvents()
    assert chip.isVisible()

    # it fades on its own, independent of the fullscreen auto-hide
    assert dialog._overlay._chip_timer.isActive()
    dialog._overlay._chip_timer.stop()
    dialog.close()
    app.processEvents()


def test_stream_chip_sits_in_the_top_left_corner(app):
    from mikai_ui.player_controller import PlaybackState, PlaybackStatus, StreamInfo
    from mikai_ui.player_dialog import PlayerDialog

    dialog = PlayerDialog("Серія 1", "https://s.example/480/index.m3u8")
    dialog.show()
    app.processEvents()
    dialog._overlay.set_playback_state(
        PlaybackState(
            status=PlaybackStatus.PLAYING, available=True, stream=StreamInfo(1280, 720, 24.0)
        )
    )
    dialog._overlay.show_controls()
    app.processEvents()

    chip = dialog._overlay.stream_chip
    stage = dialog._stage
    assert chip.isVisible()
    assert chip.geometry().left() < stage.width() // 4, "the chip belongs top-left"
    assert chip.geometry().top() < stage.height() // 4
    dialog.close()
    app.processEvents()


def test_stream_chip_reports_the_decoded_stream_not_the_selected_quality(app):
    """The quality combo is a request; the chip is what mpv actually decoded."""
    from mikai_ui.player_dialog import PlayerDialog

    dialog = PlayerDialog(
        "Серія 1",
        "https://s.example/1080/index.m3u8",
        qualities={1080: "u1080", 720: "u720", 480: "u480"},
        initial_height=1080,
    )
    dialog.show()
    app.processEvents()
    assert dialog._quality_picker.currentText() == "1080p"

    adapter = FakeMpvAdapter()
    dialog.controller.attach_adapter(adapter)
    dialog.controller.load()
    # the provider served something else entirely
    adapter.emit_stream(
        width=720,
        height=480,
        fps=0.0,
        estimated_fps=23.976,
        video_bitrate=2_400_000,
        cache_speed=3_150_000,
    )
    dialog._overlay.show_controls()
    app.processEvents()

    assert dialog._overlay.stream_line.text() == "720x480 | 23.98fps"
    assert dialog._overlay.stream_rate.text() == "2.4 Mb/s · 3.1 Mb/s"
    assert dialog._quality_picker.currentText() == "1080p"
    dialog.close()
    app.processEvents()


def test_controls_stay_up_while_a_menu_is_open(app):
    """A menu runs its own event loop, so the auto-hide used to fire underneath it
    and grey out the control the user was operating."""
    from PySide6.QtCore import QPoint
    from PySide6.QtWidgets import QApplication

    from mikai_ui.player_dialog import PlayerDialog

    dialog = PlayerDialog("Серія 1", "https://s.example/720.m3u8")
    dialog.show()
    app.processEvents()
    controls = dialog._controls
    controls.set_variants(
        [
            {"label": "Озвучення — Inari · Ashdi", "embed": "a"},
            {"label": "Субтитри — Цукіхана · Ashdi", "embed": "s"},
        ],
        "a",
    )
    dialog._overlay.set_fullscreen(True)
    dialog._overlay.show_controls()
    app.processEvents()

    menu = controls._variant_menu()
    menu.popup(controls.mapToGlobal(QPoint(0, controls.height())))
    app.processEvents()
    assert QApplication.activePopupWidget() is not None, "the menu must be up"

    dialog._overlay.hide_controls()
    # wait past the fade duration: without the guard the bar would be gone by now
    QTest.qWait(400)
    app.processEvents()
    assert controls.isVisible(), "the bar must not hide under an open menu"
    assert controls.isEnabled(), "and it must not grey out"

    # close() does not unregister the popup synchronously; drain it explicitly
    menu.close()
    while QApplication.activePopupWidget() is not None:
        QApplication.activePopupWidget().close()
        app.processEvents()
    assert QApplication.activePopupWidget() is None

    dialog._overlay.hide_controls()
    # the bar fades out; one processEvents() does not finish the animation
    QTest.qWait(400)
    app.processEvents()
    assert not controls.isVisible(), "once the menu is closed the bar hides again"
    dialog.close()
    app.processEvents()


def test_hiding_the_controls_does_not_resize_the_video_surface(app):
    """The controls share a layout with the native mpv surface, so animating
    their height resized the video window on every animation frame and stuttered."""
    from mikai_ui.player_dialog import PlayerDialog

    dialog = PlayerDialog("Серія 1", "https://s.example/720.m3u8")
    dialog.show()
    app.processEvents()
    dialog._overlay.set_fullscreen(True)
    dialog._overlay.show_controls()
    app.processEvents()

    resizes: list[int] = []
    original = dialog._video.resizeEvent

    def counting(event):
        resizes.append(1)
        original(event)

    dialog._video.resizeEvent = counting
    dialog._overlay.hide_controls()
    QTest.qWait(400)
    app.processEvents()

    assert not dialog._controls.isVisible()
    assert len(resizes) <= 1, f"video resized {len(resizes)} times while hiding"
    dialog.close()
    app.processEvents()


def test_skip_buttons_carry_a_glyph_and_the_seconds(app):
    """The skip controls were bare "−10" text; a glyph reads as skip at a glance."""
    from PySide6.QtCore import QSize

    from mikai_ui.player_controls import PlayerControlsWidget

    controls = PlayerControlsWidget()
    for button in (controls.rewind_button, controls.forward_button):
        assert not button.icon().isNull(), "the skip glyph must be painted"
        assert button.text() == "10"
        assert button.toolTip()
    assert not controls.rewind_button.icon().pixmap(QSize(18, 18)).toImage().isNull()
    controls.deleteLater()


def test_timeline_spans_the_bar_and_hovers_show_the_target_time(app):
    """The scrub target used to stop short of the right edge because the time
    readout sat in the same row."""
    from PySide6.QtCore import QPoint
    from PySide6.QtTest import QTest
    from PySide6.QtWidgets import QToolTip

    from mikai_ui.player_controls import PlayerControlsWidget

    controls = PlayerControlsWidget()
    controls.resize(1000, 120)
    controls.set_playback_state(
        PlaybackState(status=PlaybackStatus.PLAYING, available=True, duration=1440.0)
    )
    controls.show()
    app.processEvents()

    margins = controls.layout().contentsMargins()
    content = controls.width() - margins.left() - margins.right()
    assert controls.timeline.width() >= content - 2, controls.timeline.width()

    QTest.mouseMove(controls.timeline, pos=QPoint(controls.timeline.width() // 2, 10))
    app.processEvents()
    QTest.qWait(60)
    app.processEvents()
    # 50% of a 24-minute video
    assert QToolTip.text() == "12:00", QToolTip.text()
    controls.deleteLater()


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
