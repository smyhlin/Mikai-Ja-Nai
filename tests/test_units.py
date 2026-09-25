import base64
import subprocess
import sys
import time
from dataclasses import replace
from pathlib import Path
from typing import Any

import pytest
import requests
from conftest import (
    episode_button,
    pagination_controls,
    pump_until,
    quality_buttons,
    tab_chips,
)
from PySide6.QtCore import Qt
from PySide6.QtGui import QGuiApplication, QPixmap
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QLabel, QPushButton, QScrollArea, QWidget

from mikai_backend import (
    AnimeDetail,
    AnimeRelation,
    AnimeRepository,
    AnimeSummary,
    AnimeUpdate,
    AiringEntry,
    Episode,
    ImageVariant,
    MikaiClient,
    PlayerResult,
    PosterSet,
    Release,
    SearchResult,
    ScheduleResult,
    Source,
    Team,
    Titles,
    UpdatesResult,
    UaPoster,
    streams,
)
from mikai_backend.releases import sort_releases
from mikai_ui.main import MainWindow
from mikai_ui.workers import Runner

PNG_1PX = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
)


def _release(kind: str, name: str, numbers: list[int]) -> Release:
    return Release(
        id=f"t_{name}:{kind}",
        kind=kind,
        teams=[Team(id=1, slug=name.lower(), name=name)],
        episodes_count=max(numbers) if numbers else 0,
        episodes=[
            Episode(
                number=n,
                label=str(n),
                sources=[
                    Source(provider="ashdi", embed_url=f"https://ashdi.vip/vod/{name}/{n}"),
                    Source(provider="moon", embed_url=f"https://moonanime.art/iframe/{name}/{n}"),
                    Source(provider="tortuga", embed_url=f"https://tortuga.tw/vod/{name}/{n}"),
                ],
            )
            for n in numbers
        ],
    )


BLACK = AnimeSummary(
    ref="7428",
    titles=Titles(ua="Чорний смолоскип", english="BLACK TORCH"),
    year=2026,
    format="tv",
    poster=PosterSet(big=ImageVariant(webp="img:poster"), small=ImageVariant(webp="img:black")),
)
JACK = AnimeSummary(
    ref="5365",
    titles=Titles(ua="Чорний Джек"),
    year=1993,
    format="ova",
    poster=PosterSet(small=ImageVariant(webp="img:jack")),
)
NO_POSTER = AnimeSummary(ref="9999", titles=Titles(ua="Без постера"), year=2021, format="movie")

DETAIL = AnimeDetail(
    ref="7428",
    titles=Titles(ua="Чорний смолоскип", english="BLACK TORCH"),
    description="Тестовий опис. " * 20,
    format="tv",
    start_date="2026-07-04",
    poster=PosterSet(big=ImageVariant(webp="img:poster")),
)

PLAYER = PlayerResult(
    releases=[
        _release("sub", "SubTeam", [1, 2, 3, 4, 5, 6]),
        _release("voice", "Quiet", [1, 2]),
        _release("voice", "Loud", list(range(1, 25))),
        _release("voice", "Mid", [1, 2, 3, 4]),
    ]
)


class FakeRepository:
    def __init__(self) -> None:
        self.images = {"img:black": PNG_1PX, "img:jack": PNG_1PX, "img:poster": PNG_1PX}
        self.last_query: str | None = None
        self.api_key: str | None = "mk_fake_initial"

    def search(self, query: str, page: int = 1, limit: int = 20) -> SearchResult:
        self.last_query = query
        return SearchResult(total=3, items=[BLACK, JACK, NO_POSTER])

    def get_anime(self, ref: str) -> AnimeDetail:
        assert ref == "7428", ref
        return DETAIL

    def get_player(self, ref: str) -> PlayerResult:
        assert ref == "7428", ref
        return PLAYER

    def fetch_bytes(self, url: str) -> bytes:
        return self.images[url]

    def browse_ongoing(self, page: int = 1, limit: int = 24) -> SearchResult:
        return self.search("", page=page, limit=limit)

    def browse_popular(self, page: int = 1, limit: int = 18) -> SearchResult:
        return self.search("", page=page, limit=limit)

    def get_updates(self, page: int = 1, limit: int = 12) -> UpdatesResult:
        return UpdatesResult()

    def get_schedule(self) -> ScheduleResult:
        return ScheduleResult()

    def get_api_key(self) -> str | None:
        return self.api_key

    def set_api_key(self, api_key: str | None) -> None:
        self.api_key = api_key

    def validate_api_key(self, api_key: str | None) -> dict[str, Any]:
        if not api_key:
            return {"valid": True, "authenticated": False, "rate_limit": 60, "name": ""}
        if api_key == "mk_valid":
            return {"valid": True, "authenticated": True, "rate_limit": 300, "name": "tester"}
        return {"valid": False, "error": "invalid api key"}

    def rate_limit_state(self) -> dict[str, int | None]:
        return {
            "limit": 300,
            "used": 42,
            "remaining": 258,
            "reset_in": 17,
            "queued": 2,
            "in_flight": 1,
        }


@pytest.fixture
def fake_window(app):
    win = MainWindow(FakeRepository())
    win.show()
    app.processEvents()
    yield win
    win._runner._pool.waitForDone(10000)
    win.close()
    win.deleteLater()
    app.processEvents()


def open_fake_title(win, app):
    win.open_title("7428", None)
    title = win._title
    assert pump_until(app, lambda: title._selector.count() == 4), "title load stuck"
    return title


def test_fake_satisfies_repository_protocol():
    assert isinstance(FakeRepository(), AnimeRepository)


def test_backend_source_has_no_gui_imports():
    backend = Path(__file__).resolve().parent.parent / "mikai_backend"
    files = sorted(backend.glob("*.py"))
    assert files, "backend package missing"
    for path in files:
        for line in path.read_text(encoding="utf-8").splitlines():
            stripped = line.strip()
            if stripped.startswith(("import ", "from ")):
                lowered = stripped.lower()
                assert "pyside" not in lowered and "pyqt" not in lowered, (path.name, line)


def test_backend_loads_without_qt_runtime():
    code = (
        "import mikai_backend, mikai_backend.streams, sys; "
        "print([m for m in sys.modules if 'PySide' in m or 'shiboken' in m])"
    )
    proc = subprocess.run(
        [sys.executable, "-c", code],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    assert proc.returncode == 0, proc.stderr
    assert proc.stdout.strip() == "[]", proc.stdout


def test_client_parses_home_updates_and_schedule():
    client = MikaiClient(api_key="test")
    payloads = {
        "/updates": {
            "ok": True,
            "total": 1,
            "result": [
                {
                    "anime": {
                        "ids": {"mikai": 7428},
                        "titles": {"ua": "Чорний смолоскип"},
                        "images": {"poster": {"big": {"webp": "poster.webp"}}},
                    },
                    "teams": [{"id": 4, "slug": "fanvoxua", "name": "FanVoxUA"}],
                    "teamName": "FanVoxUA",
                    "isCollab": False,
                    "kind": "voice",
                    "provider": "ashdi",
                    "episode": 13,
                    "publishedAt": "2026-09-24T17:30:00Z",
                }
            ],
        },
        "/schedule": {
            "ok": True,
            "result": {
                "monday": [
                    {
                        "anime": {"ids": {"mikai": 7428}, "titles": {"ua": "Чорний смолоскип"}},
                        "episode": 14,
                        "airing": "2026-09-28T06:30:00Z",
                        "finished": False,
                    }
                ]
            },
        },
    }
    client._get = lambda path, _params=None: payloads[path]
    updates = client.get_updates()
    schedule = client.get_schedule()
    assert updates.total == 1
    assert updates.items[0].anime.ref == "7428"
    assert updates.items[0].teams[0].name == "FanVoxUA"
    assert updates.items[0].published_at.endswith("Z")
    assert schedule.days["monday"][0].episode == 14
    assert schedule.days["monday"][0].anime.titles.display_name == "Чорний смолоскип"


def test_stream_site_detection():
    assert streams.detect_site("https://ashdi.vip/vod/268104") == "ashdi"
    assert streams.detect_site("https://ashdi.vip/vod/113749") == "ashdi"
    assert streams.detect_site("https://tortuga.tw/vod/98249") == "tortuga"
    assert streams.detect_site("https://moonanime.art/iframe/n7p0wo428uufidanqjswp1u8/") == "moon"
    with pytest.raises(ValueError):
        streams.detect_site("https://example.com/episode")


def test_stream_fetch_retries_temporary_gateway_errors(monkeypatch):
    import requests

    class Response:
        def __init__(self, status: int, text: str = "ok") -> None:
            self.status_code = status
            self.text = text
            self.headers = {}

        def raise_for_status(self):
            if self.status_code >= 400:
                raise requests.HTTPError(f"HTTP {self.status_code}")

    class Session:
        def __init__(self) -> None:
            self.responses = [Response(502), Response(200, "playlist")]
            self.calls = 0

        def get(self, *args, **kwargs):
            self.calls += 1
            return self.responses.pop(0)

    session = Session()
    monkeypatch.setattr(streams.time, "sleep", lambda _delay: None)
    assert streams.fetch_text(session, "https://ashdi.vip/vod/test") == "playlist"
    assert session.calls == 2


def test_moon_empty_source_raises_stream_error(monkeypatch):
    import base64

    import requests

    def moon_encode(payload: bytes, key: bytes = b"A" * 32, seed: int = 42) -> bytes:
        buf = bytearray(1 + 32 + len(payload))
        buf[0] = seed
        buf[1:33] = key
        k = seed
        for i in range(len(payload)):
            enc_b = payload[i] ^ key[i % 32] ^ k
            buf[33 + i] = enc_b
            k = (enc_b + key[i % 32]) & 255
        return bytes(buf)

    encoded = base64.b64encode(moon_encode(b'var rawVideo = "";\n' + b" " * 100)).decode()

    class FakeResp:
        text = f'atob("{encoded}")'

        def raise_for_status(self):
            pass

    session = requests.Session()
    monkeypatch.setattr(session, "get", lambda *args, **kwargs: FakeResp())
    with pytest.raises(streams.StreamError, match="no_source"):
        streams.moon_master(session, "https://moonanime.art/iframe/test/")


def test_tortuga_token_decode():
    token = (
        "dOn8++bunoSd2qGrt6WvjMSFl41yeHN6DF1HGFYpP3wpBBoGFxH3pPb4zs3B25Kur6KwtsmexMot"
        "bXF5dEpCHVckOyc/cxdbQxywtqD08cLE3t/esaz6tpSGr87GNzgqNUlEXBlUKi83IU4KXQBE=="
    )
    master = streams.tortuga_decode(token)
    assert master == (
        "https://calypso.tortuga.tw/hls/serials/danjon.meshi/s01/"
        "danjon.meshi.s01e01.amanogawa.mvo_98249/hls/index.m3u8"
    )


def test_parse_variants_and_quality_selection():
    master = """#EXTM3U
#EXT-X-STREAM-INF:RESOLUTION=1920x1080,BANDWIDTH=2128000
https://a.example/hls/1080/index.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=6211940,RESOLUTION=1280x720,CODECS="avc1.640029"
https://a.example/hls/720/index.m3u8
#EXT-X-STREAM-INF:RESOLUTION=854x480,BANDWIDTH=500000
https://a.example/hls/480/index.m3u8
#EXT-X-STREAM-INF:RESOLUTION=640x360,BANDWIDTH=300000
media_360.m3u8"""
    variants = streams.parse_variants(master, "https://a.example/hls/master.m3u8")
    assert [variant["height"] for variant in variants] == [1080, 720, 480, 360]
    assert variants[-1]["url"] == "https://a.example/hls/media_360.m3u8", variants[-1]
    assert streams.get_quality_url(variants, 720) == "https://a.example/hls/720/index.m3u8"
    assert streams.get_quality_url(variants, 1000) == "https://a.example/hls/720/index.m3u8"
    assert streams.get_quality_url(variants, 240) == "https://a.example/hls/1080/index.m3u8"
    with pytest.raises(streams.StreamError):
        streams.get_quality_url([], 720)


def test_cover_url_prefers_ua_poster():
    detail = AnimeDetail(
        poster=PosterSet(big=ImageVariant(webp="img:poster")),
        ua_posters=[UaPoster(images=PosterSet(big=ImageVariant(webp="img:ua")))],
    )
    assert detail.cover_url == "img:ua"
    assert (
        AnimeDetail(poster=PosterSet(big=ImageVariant(webp="img:poster"))).cover_url == "img:poster"
    )
    assert AnimeDetail().cover_url is None


def test_quality_buttons_copy_stream_for_height(monkeypatch, fake_window, app):
    monkeypatch.setattr(
        "mikai_ui.title_page.streams.get_quality_streams",
        lambda url: {
            1080: "https://s.example/1080.m3u8",
            720: "https://s.example/720.m3u8",
            480: "https://s.example/480.m3u8",
        },
    )
    monkeypatch.setattr(
        "mikai_ui.title_page.prepare_runtime",
        lambda: (_ for _ in ()).throw(OSError("offline test")),
    )
    title = open_fake_title(fake_window, app)
    chip = tab_chips(title)[0]
    assert pump_until(app, lambda: len(quality_buttons(chip)) == 3)
    assert [button.text() for button in quality_buttons(chip)] == [
        "1080p",
        "720p",
        "480p",
    ]
    quality_buttons(chip)[1].click()
    app.processEvents()
    assert QGuiApplication.clipboard().text() == "https://s.example/720.m3u8"
    assert "720p" in title._status.text()
    # Clicking a quality chip opens the player dialog; runtime deploy failed offline
    assert len(title._player_dialogs) == 1
    dialog = title._player_dialogs[0]
    assert pump_until(app, lambda: dialog.status_text == "Playback unavailable")
    assert dialog._overlay.details_text() == "offline test"
    # Dialog carries the full quality list for in-player switching
    assert dialog._quality_combo.count() == 3
    assert [dialog._quality_combo.itemText(i) for i in range(3)] == [
        "1080p",
        "720p",
        "480p",
    ]
    assert dialog._quality_combo.currentText() == "720p"
    assert not dialog._quality_combo.isEnabled()  # no backend when runtime fails
    title.shutdown()
    app.processEvents()


def test_player_dialog_controls(app):
    from mikai_ui.player_dialog import PlayerDialog, _fmt_time

    dlg = PlayerDialog(
        "Серія 5 · 720p",
        "https://s.example/720.m3u8",
        qualities={1080: "u1080", 720: "u720", 480: "u480"},
        initial_height=720,
    )
    assert dlg._quality_combo.count() == 3
    assert [dlg._quality_combo.itemText(i) for i in range(3)] == [
        "1080p",
        "720p",
        "480p",
    ]
    assert dlg._quality_combo.currentText() == "720p"
    assert not dlg._quality_combo.isEnabled()
    assert [dlg._speed_combo.itemText(i) for i in range(dlg._speed_combo.count())] == [
        "0.5×",
        "0.75×",
        "1×",
        "1.25×",
        "1.5×",
        "2×",
    ]
    assert dlg._speed_combo.currentText() == "1×"
    assert dlg._time_label.text() == "0:00 / --:--"
    assert dlg._vol_slider.value() == 100
    assert not dlg._pause_btn.isEnabled()
    assert not dlg._vol_slider.isEnabled()
    assert _fmt_time(3661) == "1:01:01"
    assert _fmt_time(125) == "2:05"

    # The overlay owns the reusable control bar and Qt fullscreen restores cleanly.
    dlg.show()
    app.processEvents()
    assert dlg.isFullScreen()
    assert not dlg._header.isVisible()
    assert dlg.layout().contentsMargins().left() == 0
    # The mpv surface is a native window: any widget spanning it is composited on
    # top and hides the video, so nothing may overlap the video surface.
    assert dlg._video.parent() is dlg._stage
    assert dlg._chrome.parent() is dlg._stage
    assert not dlg._overlay.isVisible()
    assert dlg._video.width() > 0 and dlg._video.height() > 0
    for child in dlg._stage.children():
        if not isinstance(child, QWidget) or child is dlg._video or not child.isVisible():
            continue
        assert not child.geometry().contains(dlg._video.rect())
    QTest.qWait(220)
    assert dlg._chrome.geometry().height() >= dlg._chrome.sizeHint().height()
    dlg._overlay.hide_controls()
    QTest.qWait(220)
    assert not dlg._chrome.isVisible()
    dlg._overlay.show_controls()
    assert dlg._chrome.isVisible()
    assert dlg._overlay._hide_timer.isActive()
    dlg._overlay._hide_timer.stop()
    dlg._fullscreen.toggle()
    app.processEvents()
    assert not dlg.isFullScreen()
    assert dlg._header.isVisible()
    dlg._fullscreen.toggle()
    app.processEvents()
    assert dlg.isFullScreen()
    assert not dlg._header.isVisible()
    dlg.close()
    app.processEvents()


def test_fullscreen_manager_restores_window(app):
    from PySide6.QtWidgets import QWidget

    from mikai_ui.fullscreen_manager import FullscreenManager

    window = QWidget()
    window.setGeometry(80, 90, 720, 480)
    window.show()
    manager = FullscreenManager(window)
    manager.enter()
    app.processEvents()
    assert manager.is_fullscreen
    manager.exit()
    app.processEvents()
    assert not manager.is_fullscreen
    assert window.geometry().width() == 720
    assert window.geometry().height() == 480
    window.close()


def test_video_click_reveals_controls_and_toggles_pause(app):
    from conftest import FakeMpvAdapter

    from mikai_ui.player_dialog import PlayerDialog

    dlg = PlayerDialog("Серія 1", "https://s.example/720.m3u8")
    adapter = FakeMpvAdapter()
    dlg.controller.attach_adapter(adapter)
    dlg.controller.load()
    adapter.durationChanged.emit(50)
    adapter.positionChanged.emit(5)
    adapter.mediaLoaded.emit()
    dlg.show()
    app.processEvents()

    dlg._overlay.hide_controls()
    QTest.qWait(220)
    assert not dlg._chrome.isVisible()
    dlg._overlay.show_controls()
    assert dlg._chrome.isVisible()
    QTest.mouseClick(dlg._stage, Qt.MouseButton.LeftButton)
    QTest.qWait(250)
    assert adapter.pauses[-1] is True
    assert dlg.controller.state.paused
    dlg.close()
    app.processEvents()


def test_player_seek_skips_absolutely_within_bounds(app):
    from conftest import FakeMpvAdapter

    from mikai_ui.player_dialog import PlayerDialog

    dlg = PlayerDialog("Серія 1", "https://s.example/720.m3u8")
    adapter = FakeMpvAdapter()
    dlg.controller.attach_adapter(adapter)
    dlg.controller.load()
    adapter.durationChanged.emit(100)
    adapter.positionChanged.emit(30)
    adapter.mediaLoaded.emit()

    dlg.controller.rewind()
    assert adapter.seeks[-1] == 20.0
    dlg.controller.forward()
    assert adapter.seeks[-1] == 30.0
    dlg.controller.rewind(999)
    assert adapter.seeks[-1] == 0.0
    dlg.controller.forward(999)
    assert adapter.seeks[-1] == 100.0

    adapter.seeks.clear()
    dlg.controller.begin_seek()
    dlg.controller.preview_seek(55)
    dlg.controller.preview_seek(70)
    assert adapter.seeks == []
    dlg.controller.finish_seek(70)
    assert adapter.seeks == [70.0]
    dlg.close()
    app.processEvents()


def test_episode_context_menu_actions(fake_window, app, monkeypatch):
    monkeypatch.setattr(
        "mikai_ui.title_page.streams.get_quality_streams",
        lambda url: {
            1080: "https://s.example/1080.m3u8",
            720: "https://s.example/720.m3u8",
            480: "https://s.example/480.m3u8",
        },
    )
    title = open_fake_title(fake_window, app)
    widget = tab_chips(title)[0]
    assert pump_until(app, lambda: len(quality_buttons(widget)) == 3)
    chip = widget._chip

    menu = title._build_episode_menu(chip)
    texts = [action.text() for action in menu.actions()]
    assert texts[0] == "Копіювати в буфер обміну"
    assert "Відкрити в браузері" in texts
    assert "Відкрити у зовнішньому плеєрі" in texts
    submenu_action = next(a for a in menu.actions() if a.menu() is not None)
    assert submenu_action.text() == "Копіювати якість"
    assert [a.text() for a in submenu_action.menu().actions()] == [
        "1080p",
        "720p",
        "480p",
    ]

    # Copy action writes the embed URL
    menu.actions()[0].trigger()
    app.processEvents()
    assert QGuiApplication.clipboard().text() == widget.property("embed")

    # Per-quality action writes that quality's stream URL
    submenu_action.menu().actions()[1].trigger()
    app.processEvents()
    assert QGuiApplication.clipboard().text() == "https://s.example/720.m3u8"
    assert "720p" in title._status.text()

    launched = []
    monkeypatch.setattr(title, "_find_external_player", lambda: ("C:/apps/mpv.exe", "mpv"))
    monkeypatch.setattr(
        title,
        "_start_external_process",
        lambda program, arguments: launched.append((program, arguments)) or True,
    )
    external_action = next(
        action for action in menu.actions() if action.text() == "Відкрити у зовнішньому плеєрі"
    )
    external_action.trigger()
    assert launched == [("C:/apps/mpv.exe", ["https://s.example/1080.m3u8"])]
    assert "mpv · 1080p" in title._status.text()

    title.shutdown()
    app.processEvents()
    menu.deleteLater()


def test_episode_context_menu_wiring_and_placeholder(fake_window, app, monkeypatch):
    from PySide6.QtGui import QContextMenuEvent
    from PySide6.QtWidgets import QMenu as RealMenu

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})
    captured = []

    class FakeMenu(RealMenu):
        def exec(self, *args, **kwargs):
            captured.append(self)

    monkeypatch.setattr("mikai_ui.title_page.QMenu", FakeMenu)

    title = open_fake_title(fake_window, app)
    widget = tab_chips(title)[0]
    assert pump_until(app, lambda: widget._chip.get("requested") is True)

    # A real right-click context menu event drives the chip's handler
    event = QContextMenuEvent(
        QContextMenuEvent.Reason.Mouse,
        widget.rect().center(),
        widget.mapToGlobal(widget.rect().center()),
    )
    app.sendEvent(widget, event)
    app.processEvents()
    assert captured, "context menu handler did not run on RMB"
    menu = captured[0]
    texts = [action.text() for action in menu.actions()]
    assert texts[0] == "Копіювати в буфер обміну"
    assert "Відкрити в браузері" in texts
    submenu_action = next(a for a in menu.actions() if a.menu() is not None)
    items = submenu_action.menu().actions()
    assert len(items) == 1
    assert items[0].text() == "Ще не завантажено"
    assert not items[0].isEnabled()

    title.shutdown()
    app.processEvents()
    menu.deleteLater()


def test_mpv_runtime_finds_library(tmp_path):
    from mikai_backend import mpv_runtime

    (tmp_path / "bin").mkdir()
    dll = tmp_path / "bin" / "libmpv-2.dll"
    dll.write_bytes(b"MZ")

    assert mpv_runtime.find_library(tmp_path) == dll
    runtime = mpv_runtime.find_runtime(tmp_path)
    assert runtime == tmp_path.resolve()
    # a real CDLL load needs an actual DLL; covered by the live deploy check


def test_sort_releases_tie_break_by_team_name():
    first = _release("voice", "Zed", [1])
    second = _release("voice", "Amy", [1])
    assert [r.teams[0].name for r in sort_releases([first, second])] == ["Amy", "Zed"]


def test_search_lists_fake_titles_thumbs_and_format_tabs(fake_window, app):
    page = fake_window._search
    page._query.setText("чорний")
    page._search()
    assert pump_until(app, lambda: page._results.count() == 3), page._status.text()
    assert page._results.item(0).text().startswith("Чорний смолоскип")
    bar = page._format_bar
    labels = [bar.tabText(i) for i in range(bar.count())]
    assert labels == ["Усі (3)", "TV (1)", "MOVIE (1)", "OVA (1)"], labels
    assert pump_until(app, lambda: not page._results.item(0).icon().isNull())
    bar.setCurrentIndex(3)
    app.processEvents()
    rows = [page._results.item(i).text() for i in range(page._results.count())]
    assert len(rows) == 1 and rows[0].endswith("OVA"), rows
    bar.setCurrentIndex(0)
    app.processEvents()
    assert page._results.count() == 3


def test_ongoing_catalog_can_open_the_next_page(app):
    class PagedRepository(FakeRepository):
        def __init__(self) -> None:
            super().__init__()
            self.requested_pages: list[int] = []

        def browse_ongoing(self, page: int = 1, limit: int = 24) -> SearchResult:
            self.requested_pages.append(page)
            base = BLACK if page == 1 else JACK
            return SearchResult(
                total=48,
                page=page,
                pages=2,
                items=[replace(base, ref=f"{base.ref}-{index}") for index in range(18)],
            )

        def browse_popular(self, page: int = 1, limit: int = 18) -> SearchResult:
            return SearchResult(
                total=48,
                page=page,
                pages=3,
                items=[replace(JACK, ref=f"popular-{index}") for index in range(18)],
            )

    repository = PagedRepository()
    window = MainWindow(repository)
    window.show()
    try:
        assert pump_until(
            app,
            lambda: (
                len(window._search._carousel_entries.get("ongoing", [])) == 18
                and len(window._search._carousel_entries.get("popular", [])) == 18
            ),
        )
        assert window._search._carousel_entries["ongoing"][0].ref == f"{BLACK.ref}-0"
        assert window._search._carousel_entries["popular"][0].ref == "popular-0"
        assert window._search._results.isHidden()
        assert window._search._ongoing_carousel._carousel_cards.count() == 18
        assert window._search._popular_carousel._carousel_cards.count() == 18
        assert window._search._ongoing_carousel._carousel_scroll.horizontalScrollBar().maximum() > 0
        assert repository.requested_pages == [1]
    finally:
        window._runner._pool.waitForDone(10000)
        window.close()
        window.deleteLater()
        app.processEvents()


def test_home_feed_renders_recent_updates_and_schedule(app):
    from mikai_ui.main import SearchPage

    class HomeRepository(FakeRepository):
        def get_updates(self, page: int = 1, limit: int = 12) -> UpdatesResult:
            assert page == 1 and limit == 12
            return UpdatesResult(
                items=[
                    AnimeUpdate(
                        anime=BLACK,
                        team_name="FanVoxUA",
                        kind="voice",
                        provider="ashdi",
                        episode=13,
                        published_at="2026-09-24T17:30:00Z",
                    )
                ]
            )

        def get_schedule(self) -> ScheduleResult:
            return ScheduleResult(
                days={
                    "monday": [
                        AiringEntry(
                            anime=JACK,
                            episode=4,
                            airing="2026-09-28T06:30:00Z",
                        )
                    ]
                }
            )

    page = SearchPage(Runner(), HomeRepository())

    # Keep this UI test hermetic: avoid reading or writing the user's settings.
    class MemorySettings:
        def __init__(self):
            self.values = {}

        def value(self, key, default=None, *_args):
            return self.values.get(key, default)

        def setValue(self, key, value):
            self.values[key] = value

    page._settings = MemorySettings()
    page.record_recent_title(BLACK.ref, BLACK.titles.display_name, BLACK.poster_url or "")
    page.show()
    try:
        assert pump_until(
            app,
            lambda: (
                page._feed_rows["updates"].count() == 1 and page._feed_rows["schedule"].count() == 1
            ),
        )
        assert page._recent_carousel.isVisible()
        assert page._carousel_entries["recent"][0].ref == BLACK.ref
        assert page._feed_rows["updates"].itemAt(0).widget().findChild(QPushButton, "homeFeedTitle")
        assert (
            "FanVoxUA"
            in page._feed_rows["updates"]
            .itemAt(0)
            .widget()
            .findChild(QLabel, "homeFeedMeta")
            .text()
        )
        assert (
            "Серія 4"
            in page._feed_rows["schedule"]
            .itemAt(0)
            .widget()
            .findChild(QLabel, "homeFeedMeta")
            .text()
        )
        assert BLACK.ref in page._settings.values["home/recentTitles"]
    finally:
        page._runner._pool.waitForDone(10000)
        page.close()
        page.deleteLater()
        app.processEvents()


def test_carousel_tries_larger_poster_when_thumbnail_is_unavailable(fake_window):
    page = fake_window._search
    anime = replace(
        BLACK,
        poster=PosterSet(
            small=ImageVariant(webp="img:missing-small"),
            medium=ImageVariant(jpg="img:missing-medium"),
            big=ImageVariant(webp="img:poster"),
        ),
    )
    images = page._fetch_carousel_posters([anime])
    assert images == {"img:missing-small": PNG_1PX}


def test_shared_carousel_poster_updates_cards_in_both_feeds(fake_window, app):
    page = fake_window._search
    assert pump_until(app, lambda: bool(page._carousel_entries.get("ongoing")))
    result = SearchResult(items=[BLACK])
    page._show_carousel("ongoing", result)
    page._show_carousel("popular", result)
    for key in ("ongoing", "popular"):
        label = page._carousel_labels[key][0]
        label.setPixmap(QPixmap())
        label.setText("Постер недоступний")
    page._apply_carousel_thumbs({"img:black": PNG_1PX})
    assert all(
        label.text() == "" and label.pixmap() is not None and not label.pixmap().isNull()
        for key in ("ongoing", "popular")
        for label in page._carousel_labels[key]
    )


def test_related_title_returns_to_previous_title(app):
    class RelatedRepository(FakeRepository):
        def get_anime(self, ref: str) -> AnimeDetail:
            if ref == BLACK.ref:
                related = [JACK] + [
                    replace(JACK, ref=f"5365-{index}", titles=Titles(ua=f"Тайтл {index}"))
                    for index in range(2, 7)
                ]
                relations = [AnimeRelation(kind="sequel", anime=anime) for anime in related]
                similar = [replace(JACK, ref="similar-1")]
                return replace(DETAIL, relations=relations, similar=similar)
            return replace(DETAIL, ref=ref, titles=JACK.titles, relations=[])

        def get_player(self, ref: str) -> PlayerResult:
            return PLAYER if ref == BLACK.ref else PlayerResult()

        def browse_popular(self, page: int = 1, limit: int = 18) -> SearchResult:
            return SearchResult(items=[replace(JACK, ref=f"popular-{index}") for index in range(6)])

    window = MainWindow(RelatedRepository())
    window.show()
    try:
        window.open_title(BLACK.ref, None)
        assert pump_until(app, lambda: window._title._heading.text() == BLACK.titles.display_name)
        assert window._title._related_button.isVisible()
        assert window._title._related_button.isChecked()
        assert window._title._related_host.isVisible()
        assert pump_until(
            app, lambda: len(window._title.findChildren(QScrollArea, "animeCarousel")) == 3
        )
        headings = [
            label.text()
            for label in window._title.findChildren(QLabel)
            if label.property("carouselHeading")
        ]
        assert headings == ["Пов’язані тайтли", "Схожі аніме", "Рекомендації на Mikai"]
        related = window._title.findChild(QPushButton, "animeCard")
        assert related is not None
        carousel = window._title.findChild(QScrollArea, "animeCarousel")
        assert carousel is not None
        assert pump_until(app, lambda: carousel.horizontalScrollBar().maximum() > 0)
        controls = window._title.findChildren(QPushButton, "carouselControl")
        assert len(controls) == 6 and controls[1].isEnabled()
        previous_offset = carousel.horizontalScrollBar().value()
        controls[1].click()
        assert carousel.horizontalScrollBar().value() > previous_offset
        related.click()
        assert pump_until(app, lambda: window._title_ref == JACK.ref)
        window.go_search()
        assert pump_until(app, lambda: window._title_ref == BLACK.ref)
        window.go_search()
        assert window._stack.currentWidget() is window._search
    finally:
        window._runner._pool.waitForDone(10000)
        window.close()
        window.deleteLater()
        app.processEvents()


def test_title_overview_and_episode_grid_resize(fake_window, app):
    title = open_fake_title(fake_window, app)
    preview = title._description.text()
    assert preview.endswith("…")
    assert not title._description_expanded
    assert title._description_button.text() == "Читати далі"
    assert not title._meta_host.isVisible()
    title._description_button.click()
    assert title._description.text() == DETAIL.description.strip()
    assert title._description_expanded
    assert title._description_button.text() == "Згорнути опис"
    title._description_button.click()
    assert title._description.text() == preview
    assert not title._description_expanded
    title._meta_button.click()
    assert title._meta_host.isVisible()

    fake_window.resize(860, 620)
    app.processEvents()
    narrow_columns = title._tab_states[0]["grid"]._columns
    fake_window.resize(1600, 900)
    app.processEvents()
    grid = title._tab_states[0]["grid"]
    wide_columns = grid._columns
    assert 1 <= narrow_columns <= 2
    assert wide_columns > narrow_columns
    first_row_card = grid.layout().itemAt(0).widget()
    second_row_card = grid.layout().itemAt(wide_columns).widget()
    last_card = grid.layout().itemAt(11).widget()
    assert second_row_card.y() - first_row_card.y() <= first_row_card.height() + 16
    assert grid.height() - last_card.geometry().bottom() < 24
    assert title._tabs.height() < 360
    assert title.height() - title._tabs.geometry().bottom() <= 24
    assert title._overview_scroll.height() > 292


def test_anonymous_key_check_does_not_use_saved_key(monkeypatch):
    client = MikaiClient(api_key="mk_saved")
    seen_headers = []

    class Response:
        def json(self):
            return {"ok": True, "result": {"authenticated": False, "rateLimitPerMin": 60}}

    def fake_get(_url, **kwargs):
        seen_headers.append(kwargs["headers"])
        return Response()

    monkeypatch.setattr(requests, "get", fake_get)
    monkeypatch.setattr(
        client._session, "get", lambda *_args, **_kwargs: pytest.fail("saved session used")
    )
    assert client.validate_api_key(None)["authenticated"] is False
    assert seen_headers == [{}]
    assert client.rate_limit_state()["used"] is None


def test_rate_limit_retry_waits_for_server_reset(monkeypatch):
    client = MikaiClient(api_key="mk_test")
    attempts: list[float] = []

    class Response:
        reason = "OK"

        def __init__(self, status_code: int, remaining: int, reset: int) -> None:
            self.status_code = status_code
            self.headers = {
                "X-RateLimit-Limit": "300",
                "X-RateLimit-Remaining": str(remaining),
                "X-RateLimit-Reset": str(reset),
                "Retry-After": "1",
            }

        def json(self):
            return {"ok": self.status_code == 200, "result": []}

    def fake_get(*_args, **_kwargs):
        attempts.append(time.monotonic())
        return Response(429, 0, 1) if len(attempts) == 1 else Response(200, 299, 60)

    monkeypatch.setattr(client._session, "get", fake_get)
    assert client._get("/anime")["ok"] is True
    assert len(attempts) == 2
    assert attempts[1] - attempts[0] >= 0.9


def test_title_sorts_releases_and_preselects_loudest_dub(fake_window, app):
    title = open_fake_title(fake_window, app)
    labels = [title._selector.itemText(i) for i in range(title._selector.count())]
    assert labels == [
        "Озвучення — Loud · 24 серій",
        "Озвучення — Mid · 4 серій",
        "Озвучення — Quiet · 2 серій",
        "Субтитри — SubTeam · 6 серій",
    ], labels
    assert title._selector.currentIndex() == 0
    assert title._heading.text() == "Чорний смолоскип"
    assert pump_until(
        app,
        lambda: title._cover.pixmap() is not None and not title._cover.pixmap().isNull(),
    )
    tabs = {title._tabs.tabText(i) for i in range(title._tabs.count())}
    assert tabs == {"Ashdi", "MoonAnime", "Tortuga"}, tabs
    assert len(tab_chips(title)) == 12
    assert labels[0] == "Озвучення — Loud · 24 серій"


def test_clicking_episode_copies_its_url(fake_window, app, monkeypatch):
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})
    title = open_fake_title(fake_window, app)
    chips = tab_chips(title)
    assert len(chips) == 12
    expected = chips[0].property("embed")
    assert expected.startswith("http"), expected
    QTest.mouseClick(episode_button(chips[0]), Qt.MouseButton.LeftButton)
    app.processEvents()
    assert title._url.text() == expected
    assert QGuiApplication.clipboard().text() == expected
    assert "скопійовано" in title._status.text().lower()
    assert pump_until(app, lambda: title._status.text() == "", timeout=5.0)
    title._copy_all.click()
    app.processEvents()
    copied = [line for line in QGuiApplication.clipboard().text().splitlines() if line]
    assert len(copied) == 24


def test_strict_player_tab_isolation_and_no_unexistent_quality_chips(fake_window, app, monkeypatch):
    monkeypatch.setattr(
        "mikai_ui.title_page.streams.get_quality_streams",
        lambda url: (
            {1080: "https://ashdi.m3u8", 720: "https://ashdi.m3u8"} if "ashdi.vip" in url else {}
        ),
    )
    title = open_fake_title(fake_window, app)
    tab_names = [title._tabs.tabText(i) for i in range(title._tabs.count())]
    assert tab_names == ["Ashdi", "MoonAnime", "Tortuga"]

    # 1. Ashdi tab: only ashdi URLs
    ashdi_chips = tab_chips(title, 0)
    assert len(ashdi_chips) == 12
    for chip in ashdi_chips:
        embed = chip.property("embed")
        assert "ashdi.vip" in embed, embed
        assert "moon" not in embed and "tortuga" not in embed

    # Ashdi quality chips: only 1080p and 720p exist, NO 480p placeholder
    assert pump_until(app, lambda: len(quality_buttons(ashdi_chips[0])) == 2)
    assert [b.text() for b in quality_buttons(ashdi_chips[0])] == ["1080p", "720p"]

    # 2. Tortuga tab: switch to Tortuga tab (index 2)
    title._tabs.setCurrentIndex(2)
    app.processEvents()
    tortuga_chips = tab_chips(title, 2)
    assert len(tortuga_chips) == 12
    for chip in tortuga_chips:
        embed = chip.property("embed")
        assert "tortuga.tw" in embed, embed
        assert "ashdi" not in embed and "moon" not in embed
    # Non-existent qualities on Tortuga -> 0 quality chips
    assert len(quality_buttons(tortuga_chips[0])) == 0

    # 3. MoonAnime tab: switch to Moon tab (index 1)
    title._tabs.setCurrentIndex(1)
    app.processEvents()
    moon_chips = tab_chips(title, 1)
    assert len(moon_chips) == 12
    for chip in moon_chips:
        embed = chip.property("embed")
        assert "moonanime.art" in embed, embed
        assert "ashdi" not in embed and "tortuga" not in embed
    assert len(quality_buttons(moon_chips[0])) == 0


def test_pagination_limits_extraction_to_twelve_episodes(fake_window, app, monkeypatch):
    extracted_urls = []

    def mock_streams(url):
        extracted_urls.append(url)
        return {720: f"{url}/720.m3u8"}

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)
    title = open_fake_title(fake_window, app)

    # Page 1: exactly 12 episodes displayed
    chips_p1 = tab_chips(title)
    assert len(chips_p1) == 12
    assert episode_button(chips_p1[0]).text() == "Серія 1"
    assert episode_button(chips_p1[-1]).text() == "Серія 12"

    prev_btn, page_lbl, next_btn = pagination_controls(title)
    assert prev_btn is not None and next_btn is not None and page_lbl is not None
    assert prev_btn.isEnabled() is False
    assert next_btn.isEnabled() is True
    assert page_lbl.text() == "1 / 2"
    # Extraction must process at most 12 episodes on this page, not all 24
    assert pump_until(app, lambda: len(extracted_urls) == 12)

    # Click Next (->)
    QTest.mouseClick(next_btn, Qt.MouseButton.LeftButton)
    app.processEvents()

    chips_p2 = tab_chips(title)
    assert len(chips_p2) == 12
    assert episode_button(chips_p2[0]).text() == "Серія 13"
    assert episode_button(chips_p2[-1]).text() == "Серія 24"
    assert prev_btn.isEnabled() is True
    assert next_btn.isEnabled() is False
    assert page_lbl.text() == "2 / 2"
    # Now exactly 24 extractions have run (12 for page 1 + 12 for page 2)
    assert pump_until(app, lambda: len(extracted_urls) == 24)

    # Click Prev (<-)
    QTest.mouseClick(prev_btn, Qt.MouseButton.LeftButton)
    app.processEvents()

    chips_back = tab_chips(title)
    assert len(chips_back) == 12
    assert episode_button(chips_back[0]).text() == "Серія 1"
    assert page_lbl.text() == "1 / 2"
    assert prev_btn.isEnabled() is False
    assert next_btn.isEnabled() is True


def test_no_source_shows_badge_and_feedback(fake_window, app, monkeypatch):
    def mock_streams(url):
        raise streams.NoSourceError(f"moonanime has no video source for {url} (no_source)")

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)
    title = open_fake_title(fake_window, app)
    chips = tab_chips(title)
    chip = chips[0]

    # Quality buttons should be 0
    assert len(quality_buttons(chip)) == 0

    # There should be a "відео відсутнє" badge inside the chip's holder
    from PySide6.QtWidgets import QLabel

    assert pump_until(
        app,
        lambda: any("відео відсутнє" in lbl.text() for lbl in chip.findChildren(QLabel)),
        timeout=5.0,
    )
    badge = next(lbl for lbl in chip.findChildren(QLabel) if "відео відсутнє" in lbl.text())
    assert "no_source" in badge.toolTip()

    # Clicking the episode button copies embed URL and informs user that video is missing
    btn = episode_button(chip)
    QTest.mouseClick(btn, Qt.MouseButton.LeftButton)
    app.processEvents()
    assert "відео відсутнє" in title._status.text()
    assert QGuiApplication.clipboard().text() == chip.property("embed")


def test_settings_chip_in_search_and_title_pages(fake_window, app):
    # Verify settings chip in search page
    search_page = fake_window._search
    assert hasattr(search_page, "_settings_btn")
    assert search_page._settings_btn.objectName() == "settingsChip"
    assert search_page._settings_btn.toolTip() == "Налаштування токена API"
    assert not search_page._settings_btn.icon().isNull()

    # Open title page and verify settings chip there as well
    title = open_fake_title(fake_window, app)
    assert hasattr(title, "_settings_btn")
    assert title._settings_btn.objectName() == "settingsChip"
    assert title._settings_btn.toolTip() == "Налаштування токена API"
    assert not title._settings_btn.icon().isNull()


def test_settings_dialog_validation_and_token_save(fake_window, app):
    from mikai_ui.settings_dialog import SettingsDialog

    repo = fake_window._repository
    dialog = SettingsDialog(repo, fake_window._runner, fake_window)
    dialog.show()
    app.processEvents()

    assert dialog._token_input.text() == "mk_fake_initial"

    # Test invalid token
    dialog._token_input.setText("mk_bad")
    dialog._test_btn.click()
    assert pump_until(app, lambda: "Помилка" in dialog._status.text())

    # Test valid token
    dialog._token_input.setText("mk_valid")
    dialog._test_btn.click()
    assert pump_until(app, lambda: "Ключ дійсний" in dialog._status.text())
    assert "300/хв" in dialog._status.text()
    assert "tester" in dialog._status.text()

    # Save token
    dialog._save_btn.click()
    app.processEvents()
    assert repo.get_api_key() == "mk_valid"


def test_settings_dialog_shows_live_quota_and_queue(fake_window, app):
    from mikai_ui.settings_dialog import SettingsDialog

    dialog = SettingsDialog(fake_window._repository, fake_window._runner, fake_window)
    assert "42" in dialog._quota.text()
    assert "258" in dialog._quota.text()
    assert "300" in dialog._quota.text()
    assert "17 с" in dialog._quota.text()
    assert "у черзі: 2" in dialog._quota.text()
