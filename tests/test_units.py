import base64
import json
import subprocess
import sys
import time
from dataclasses import replace
from pathlib import Path
from typing import Any

import pytest
import requests
from conftest import (
    FakeMpvAdapter,
    pagination_controls,
    pump_until,
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
    EpisodeStats,
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
from mikai_ui.theme.metrics import PAGER_CHIP_WINDOW
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
    win._runner.wait_for_done(10000)
    win.close()
    win.deleteLater()
    app.processEvents()


def open_fake_title(win, app):
    win.open_title("7428", None)
    title = win._title
    assert pump_until(app, lambda: title._selector.count() == 4), "title load stuck"
    return title


class _MemorySettings:
    def __init__(self) -> None:
        self.values: dict[str, object] = {}

    def value(self, key: str, default: object = None, *_args) -> object:
        return self.values.get(key, default)

    def setValue(self, key: str, value: object) -> None:
        self.values[key] = value


class _HealingRepository(FakeRepository):
    """Serves a fresh, fetchable poster for any ref, like the live API would."""

    HEALED = "https://img.example/healed.webp"

    def __init__(self) -> None:
        super().__init__()
        self.anime_lookups: list[str] = []

    def get_anime(self, ref: str) -> AnimeDetail:
        self.anime_lookups.append(ref)
        return replace(DETAIL, ref=ref, poster=PosterSet(big=ImageVariant(webp=self.HEALED)))

    def fetch_bytes(self, url: str) -> bytes:
        if url == self.HEALED:
            return PNG_1PX
        return super().fetch_bytes(url)


def test_ui_tests_never_write_the_user_settings_store(app, isolated_settings):
    window = MainWindow(FakeRepository())
    try:
        store = Path(window._search._settings.fileName())
        assert store == isolated_settings, store
        assert str(store).startswith(str(isolated_settings.parent)), store
        window._search.record_recent_title("7428", "Чорний смолоскип", "https://img.example/p.webp")
        app.processEvents()
        assert isolated_settings.exists(), "settings were not written to the isolated store"
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_unusable_poster_url_is_not_persisted(fake_window):
    page = fake_window._search
    page._settings = _MemorySettings()
    page.record_recent_title("7428", "Чорний смолоскип", "img:poster")
    assert json.loads(page._settings.values["home/recentTitles"])[0]["poster"] == ""


def test_recent_poster_is_relooked_up_when_the_stored_url_is_unusable(fake_window, app):
    """A persisted poster URL can rot; the card must recover from the API, not stay blank."""
    page = fake_window._search
    page._settings = _MemorySettings()
    repository = _HealingRepository()
    page._repository = repository
    page._thumb_cache.clear()
    page._poster_lookups.clear()
    page.record_recent_title("recent-only", "Недавній тайтл", "img:not-a-url")
    app.processEvents()

    # The heal re-renders the carousel, so look the card up on every poll
    def painted() -> bool:
        pixmap = page._carousel_widgets["recent"][0].poster.pixmap()
        return pixmap is not None and not pixmap.isNull()

    assert pump_until(app, painted), (repository.anime_lookups, len(page._thumb_cache))
    assert "recent-only" in repository.anime_lookups, repository.anime_lookups
    assert json.loads(page._settings.values["home/recentTitles"])[0]["poster"] == repository.HEALED


def test_recent_poster_lookup_runs_once_per_ref(fake_window, app):
    page = fake_window._search
    page._settings = _MemorySettings()
    repository = _HealingRepository()
    page._repository = repository
    page._thumb_cache.clear()
    page._poster_lookups.clear()
    page.record_recent_title("recent-only", "Недавній тайтл", "img:not-a-url")
    assert pump_until(app, lambda: "recent-only" in repository.anime_lookups), (
        repository.anime_lookups
    )

    for _ in range(3):
        page._show_carousel("recent", SearchResult(items=page._recent_items))
        app.processEvents()
    assert repository.anime_lookups.count("recent-only") == 1, repository.anime_lookups


def test_cover_click_opens_zoomable_viewer(fake_window, app):
    from PySide6.QtCore import QPoint, QSize

    from mikai_ui.cover_viewer import CoverLabel, CoverViewer
    from mikai_ui.theme.metrics import COVER_ZOOM_MAX, COVER_ZOOM_MIN

    title = open_fake_title(fake_window, app)
    try:
        assert isinstance(title._cover, CoverLabel)
        assert not title._cover._zoomable, "no cover means nothing to open"

        title._cover_pixmap = QPixmap(400, 600)
        title._cover.set_zoomable(True)
        QTest.mouseClick(title._cover, Qt.MouseButton.LeftButton, pos=QPoint(10, 10))
        app.processEvents()

        viewer = title._cover_viewer
        assert isinstance(viewer, CoverViewer) and viewer.isVisible()
        assert viewer._view._image.size() == QSize(400, 600)

        fitted = viewer._view._label.size()
        viewer._view.zoom_in()
        app.processEvents()
        assert viewer._view._label.width() > fitted.width()

        viewer._view.reset_zoom()
        app.processEvents()
        assert viewer._view._label.size() == fitted

        for _ in range(60):
            viewer._view.zoom_in()
        for _ in range(120):
            viewer._view.zoom_out()
        assert COVER_ZOOM_MIN <= viewer._view._zoom <= COVER_ZOOM_MAX

        viewer.close()
        app.processEvents()
        # The viewer deletes itself, so the page must not keep a dangling handle.
        assert title._cover_viewer is None
    finally:
        title.shutdown()


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


def test_player_dialog_controls(app):
    from mikai_ui.player_controls import format_time
    from mikai_ui.player_dialog import PlayerDialog

    dlg = PlayerDialog(
        "Серія 5 · 720p",
        "https://s.example/720.m3u8",
        qualities={1080: "u1080", 720: "u720", 480: "u480"},
        initial_height=720,
    )
    assert dlg._quality_picker.count() == 3
    assert [dlg._quality_picker.itemText(i) for i in range(3)] == [
        "1080p",
        "720p",
        "480p",
    ]
    assert dlg._quality_picker.currentText() == "720p"
    assert not dlg._quality_picker.isEnabled()
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
    assert format_time(3661) == "1:01:01"
    assert format_time(125) == "2:05"

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


def test_player_header_keeps_a_long_episode_name_on_one_line(app):
    from mikai_ui.player_dialog import PlayerDialog
    from mikai_ui.theme.metrics import PLAYER_MIN_HEIGHT, PLAYER_MIN_WIDTH

    caption = "Дуже довга назва серії " * 12
    dlg = PlayerDialog(caption, "https://s.example/720.m3u8")
    dlg.resize(PLAYER_MIN_WIDTH, PLAYER_MIN_HEIGHT)
    dlg.show()
    dlg._fullscreen.toggle()  # the header is windowed-only chrome
    app.processEvents()
    # A wrapped header grows the band and steals height from the video
    assert dlg._title.text() != caption
    assert dlg._title.text().endswith("\u2026")
    line_height = dlg._title.fontMetrics().height()
    assert dlg._title.height() <= line_height + 2
    dlg._fullscreen.toggle()
    dlg.close()
    app.processEvents()


def test_player_toast_sits_above_the_bar_and_clear_of_the_status_card(app):
    from mikai_ui.player_dialog import PlayerDialog

    dlg = PlayerDialog("Серія 5", "https://s.example/720.m3u8")
    dlg.show()
    dlg.show_error("потік недоступний")
    dlg._overlay.show_toast("Якість: 1080p")
    app.processEvents()
    toast = dlg._overlay.toast
    card = dlg._overlay.status_card
    assert toast.isVisible() and card.isVisible()
    # Both used to be centred on the stage, so a quality switch hid the error
    assert not toast.geometry().intersects(card.geometry())
    assert toast.geometry().bottom() <= dlg._controls.geometry().top()
    dlg.close()
    app.processEvents()


class _StubClipboard:
    """Board that rejects the first `failures` writes, like COM refusing OLE."""

    def __init__(self, failures: int = 0) -> None:
        self.failures = failures
        self.writes = 0
        self.payload = "stale content"

    def setText(self, text: str) -> None:
        self.writes += 1
        if self.writes > self.failures:
            self.payload = text

    def text(self) -> str:
        return self.payload


class _ClipboardHost:
    """Stands in for QGuiApplication inside mikai_ui.clipboard."""

    def __init__(self, board: _StubClipboard) -> None:
        self._board = board

    def clipboard(self) -> _StubClipboard:
        return self._board


def _point_clipboard_at(monkeypatch, board: _StubClipboard) -> None:
    monkeypatch.setattr("mikai_ui.clipboard.QGuiApplication", _ClipboardHost(board))


def test_copy_text_retries_a_refused_write(monkeypatch):
    from mikai_ui.clipboard import copy_text

    board = _StubClipboard(failures=1)
    _point_clipboard_at(monkeypatch, board)
    assert copy_text("https://s.example/720.m3u8") is True
    assert board.writes == 2, "one refusal must cost exactly one retry"


def test_copy_text_reports_a_dead_clipboard(monkeypatch):
    from mikai_ui.clipboard import copy_text

    board = _StubClipboard(failures=10)
    _point_clipboard_at(monkeypatch, board)
    assert copy_text("https://s.example/720.m3u8") is False
    assert board.payload == "stale content", "the old content must survive"


def test_a_refused_copy_flashes_failure_not_success(fake_window, app, monkeypatch):
    """The status line claimed "copied" while the clipboard kept its old text."""
    board = _StubClipboard(failures=10)
    _point_clipboard_at(monkeypatch, board)

    title = open_fake_title(fake_window, app)
    entry = tab_chips(title)[0]._entry
    title._copy_episode(entry)
    app.processEvents()

    assert "Не вдалося скопіювати" in title._status.text(), title._status.text()
    assert "Скопійовано" not in title._status.text()
    title.shutdown()
    app.processEvents()


def test_player_disabled_transport_glyph_is_dimmed(app):
    from PySide6.QtCore import QSize
    from PySide6.QtGui import QIcon

    from mikai_ui.player_controls import PlayerControlsWidget
    from mikai_ui.theme.metrics import ICON_SIZE

    controls = PlayerControlsWidget()
    button = controls.play_button
    size = QSize(ICON_SIZE, ICON_SIZE)
    live = button.icon().pixmap(size, QIcon.Mode.Normal, QIcon.State.Off)
    # Qt fades a disabled button's text but never its icon, so the glyph has to
    # carry a dimmed pixmap of its own or the control looks usable while inert.
    dimmed = button.icon().pixmap(size, QIcon.Mode.Disabled, QIcon.State.Off)
    assert not live.isNull()
    assert not dimmed.isNull()
    assert dimmed.toImage() != live.toImage()
    controls.deleteLater()


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
        lambda url: {1080: "https://s.example/1080.m3u8", 720: "https://s.example/720.m3u8"},
    )
    title = open_fake_title(fake_window, app)
    button = tab_chips(title)[0]
    menu = title._build_episode_menu(button._entry)
    texts = [action.text() for action in menu.actions()]
    assert texts[0] == "Копіювати в буфер обміну"
    assert "Відкрити у зовнішньому плеєрі" in texts
    assert "Відкрити в браузері" in texts
    assert not any(action.menu() for action in menu.actions()), (
        "the quality picker lives in the player, not here"
    )

    menu.actions()[0].trigger()
    app.processEvents()
    assert QGuiApplication.clipboard().text() == button.property("embed")

    launched = []
    monkeypatch.setattr(title, "_find_external_player", lambda: ("C:/apps/mpv.exe", "mpv"))
    monkeypatch.setattr(
        title,
        "_start_external_process",
        lambda program, arguments: launched.append((program, arguments)) or True,
    )
    external = next(
        action for action in menu.actions() if action.text() == "Відкрити у зовнішньому плеєрі"
    )
    external.trigger()
    app.processEvents()
    assert pump_until(app, lambda: launched), "the external launch waits for one fetch"
    assert launched == [("C:/apps/mpv.exe", ["https://s.example/1080.m3u8"])]
    assert "mpv · 1080p" in title._status.text()

    opened = []
    monkeypatch.setattr(
        "mikai_ui.title_page.QDesktopServices.openUrl", lambda url: opened.append(url)
    )
    browser = next(action for action in menu.actions() if action.text() == "Відкрити в браузері")
    browser.trigger()
    app.processEvents()
    assert opened and opened[0].toString().startswith("http")

    title.shutdown()
    app.processEvents()
    menu.deleteLater()


def test_episode_context_menu_wiring(fake_window, app, monkeypatch):
    from PySide6.QtGui import QContextMenuEvent
    from PySide6.QtWidgets import QMenu as RealMenu

    captured = []

    class FakeMenu(RealMenu):
        def exec(self, *args, **kwargs):
            captured.append(self)

    monkeypatch.setattr("mikai_ui.title_page.QMenu", FakeMenu)
    title = open_fake_title(fake_window, app)
    button = tab_chips(title)[0]
    event = QContextMenuEvent(
        QContextMenuEvent.Reason.Mouse,
        button.rect().center(),
        button.mapToGlobal(button.rect().center()),
    )
    app.sendEvent(button, event)
    app.processEvents()
    assert captured, "context menu handler did not run on RMB"
    menu = captured[0]
    assert [action.text() for action in menu.actions()][0] == "Копіювати в буфер обміну"
    assert not any(action.menu() for action in menu.actions())

    title.shutdown()
    app.processEvents()
    menu.deleteLater()


def test_search_row_context_menu_copies_link_and_ref(fake_window, app):
    from mikai_backend import anime_page_url

    page = fake_window._search
    page._on_results(SearchResult(total=1, items=[BLACK]), 1)
    assert page._results.count() == 1
    item = page._results.item(0)
    assert item.data(Qt.ItemDataRole.UserRole)["ref"] == BLACK.ref
    assert page._results.contextMenuPolicy() == Qt.CustomContextMenu

    captured: list = []
    from PySide6.QtWidgets import QMenu as RealMenu

    class FakeMenu(RealMenu):
        def exec(self, *args, **kwargs):
            captured.append(self)

    real_menu = page._results
    import mikai_ui.main as main_module

    original = main_module.QMenu
    try:
        main_module.QMenu = FakeMenu
        page._results_context_menu(real_menu.visualItemRect(item).center())
        assert captured, "menu did not open"
        menu = captured[0]
        actions = {a.text(): a for a in menu.actions()}
        assert set(actions) == {"Відкрити", "Копіювати посилання", "Копіювати ID"}

        actions["Копіювати посилання"].trigger()
        assert QGuiApplication.clipboard().text() == anime_page_url(BLACK.ref)
        actions["Копіювати ID"].trigger()
        assert QGuiApplication.clipboard().text() == BLACK.ref
    finally:
        main_module.QMenu = original
        for menu in captured:
            menu.deleteLater()


def test_carousel_card_context_menu_opens_browser(fake_window, app, monkeypatch):
    import mikai_ui.main as main_module

    opened: list = []
    monkeypatch.setattr(
        "PySide6.QtGui.QDesktopServices.openUrl", lambda url: opened.append(url) or True
    )
    page = fake_window._search
    page._show_carousel("ongoing", SearchResult(total=1, items=[BLACK]))
    cards = page._carousel_sections["ongoing"].findChildren(QPushButton, "animeCard")
    assert cards, "carousel card missing"
    assert cards[0].contextMenuPolicy() == Qt.CustomContextMenu

    captured: list = []

    class FakeMenu(main_module.QMenu):
        def exec(self, *args, **kwargs):
            captured.append(self)

    monkeypatch.setattr("mikai_ui.main.QMenu", FakeMenu)
    cards[0].customContextMenuRequested.emit(cards[0].rect().center())
    app.processEvents()
    assert captured, "card menu did not open"
    menu = captured[0]
    texts = [a.text() for a in menu.actions()]
    assert texts == ["Відкрити", "Копіювати посилання", "Копіювати ID", "Відкрити в браузері"]
    browser = next(a for a in menu.actions() if a.text() == "Відкрити в браузері")
    browser.trigger()
    from mikai_backend import anime_page_url

    assert opened[0].toString() == anime_page_url(BLACK.ref)
    menu.deleteLater()


def test_card_shows_status_and_planned_episodes_until_the_detail_arrives():
    from mikai_ui.title_card import TitleCard

    card = TitleCard(replace(BLACK, status="ongoing", episodes=12))
    assert card.status.text() == "Онгоінґ"
    assert card.episodes.text() == "12 серій"

    card.apply_detail(replace(DETAIL, status="ongoing", episodes=EpisodeStats(total=12, aired=11)))
    assert card.episodes.text() == "11 / 12 серій"


def test_visible_carousel_card_learns_its_aired_episode_count(fake_window, app):
    class OngoingRepository(FakeRepository):
        def get_anime(self, ref: str) -> AnimeDetail:
            return replace(
                DETAIL, ref=ref, status="ongoing", episodes=EpisodeStats(total=12, aired=11)
            )

    page = fake_window._search
    page._repository = OngoingRepository()
    page._show_carousel("ongoing", SearchResult(total=1, items=[replace(BLACK, episodes=12)]))
    assert page._carousel_widgets["ongoing"][0].episodes.text() == "12 серій"

    def episodes_text() -> list[str]:
        return [card.episodes.text() for card in page._carousel_widgets["ongoing"]]

    assert pump_until(app, lambda: "11 / 12 серій" in episodes_text()), episodes_text()
    assert "Онгоінґ" in [card.status.text() for card in page._carousel_widgets["ongoing"]]


def test_title_cover_context_menu_copies_and_saves(fake_window, app, monkeypatch, tmp_path):
    title = open_fake_title(fake_window, app)
    assert title._cover_url == ""
    url = "https://images.mikai.me/poster/big/cover.webp"

    pixmap = QPixmap()
    assert pixmap.loadFromData(PNG_1PX) and not pixmap.isNull()
    title._cover_pixmap = pixmap
    title._cover_url = url
    title._copy_cover_url()
    assert QGuiApplication.clipboard().text() == url

    saved: list = []
    monkeypatch.setattr(
        "PySide6.QtWidgets.QFileDialog.getSaveFileName",
        lambda *args, **kwargs: (str(tmp_path / "cover.png"), ""),
    )
    monkeypatch.setattr(
        QPixmap, "save", lambda self, path, *args, **kwargs: saved.append(path) or True
    )
    title._save_cover()
    assert saved == [str(tmp_path / "cover.png")]
    assert "збережено" in title._status.text().lower()

    def broken_save(self, path, *args, **kwargs):
        return False

    monkeypatch.setattr(QPixmap, "save", broken_save)
    title._save_cover()
    assert "не вдалося" in title._status.text().lower()

    title.shutdown()
    app.processEvents()


def test_anime_page_url_builds_public_link():
    from mikai_backend import anime_page_url

    assert anime_page_url("7428") == "https://mikai.me/anime/7428"
    assert anime_page_url("mal:21") == "https://mikai.me/anime/mal:21"


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
        window._runner.wait_for_done(10000)
        window.close()
        window.deleteLater()
        app.processEvents()


def pager_chips(page) -> list:
    layout = page._pager._chip_layout
    return [layout.itemAt(index).widget() for index in range(layout.count())]


def test_search_pager_labels_each_range_and_marks_the_active_one(app):
    class RangeRepository(FakeRepository):
        def search(self, query: str, page: int = 1, limit: int = 20) -> SearchResult:
            return SearchResult(
                total=132,
                page=page,
                # the live API counts pages at its own page size, not the 55 asked for,
                # so a 132-title answer arrives claiming 15 pages
                pages=15,
                items=[replace(BLACK, ref=f"{BLACK.ref}-{page}-{index}") for index in range(limit)],
            )

    window = MainWindow(RangeRepository())
    window.show()
    app.processEvents()
    page = window._search
    page._query.setText("Бліч")
    page._search()
    assert pump_until(app, lambda: page._results.count() > 0)
    try:
        chips = pager_chips(page)
        assert [chip.text() for chip in chips] == ["1 – 55", "56 – 110", "111 – 132"], [
            chip.text() for chip in chips
        ]
        assert chips[0].objectName() == "pagerChipActive"

        chips[2].click()
        assert pump_until(app, lambda: page._page == 3)
        chips = pager_chips(page)
        assert chips[2].objectName() == "pagerChipActive"
        assert not page._pager.next_btn.isEnabled()
        page._pager.prev_btn.click()
        assert pump_until(app, lambda: page._page == 2)
    finally:
        window._runner.wait_for_done(10000)
        window.close()
        window.deleteLater()
        app.processEvents()


def test_search_pager_jumps_to_first_last_and_a_typed_page(app):
    class RangeRepository(FakeRepository):
        def search(self, query: str, page: int = 1, limit: int = 20) -> SearchResult:
            return SearchResult(
                total=132,
                page=page,
                # the live API counts pages at its own page size, not the 55 asked for
                pages=15,
                items=[replace(BLACK, ref=f"{BLACK.ref}-{page}-{index}") for index in range(limit)],
            )

    window = MainWindow(RangeRepository())
    window.show()
    app.processEvents()
    page = window._search
    try:
        page._query.setText("Бліч")
        page._search()
        assert pump_until(app, lambda: page._results.count() > 0)
        pager = page._pager
        # three pages at 55 a page, whatever the server counted
        assert pager.jump.maximum() == 3
        assert not pager.first_btn.isEnabled()

        pager.last_btn.click()
        app.processEvents()
        assert pump_until(app, lambda: page._page == 3)
        assert not pager.last_btn.isEnabled()
        assert pager.first_btn.isEnabled()

        pager.first_btn.click()
        app.processEvents()
        assert pump_until(app, lambda: page._page == 1)

        pager.jump.setValue(2)
        app.processEvents()
        assert pump_until(app, lambda: page._page == 2)
        assert pager.jump.value() == 2
    finally:
        window._runner.wait_for_done(10000)
        window.close()
        window.deleteLater()
        app.processEvents()


def test_search_pager_scrolls_a_window_of_chips(app):
    class HugeRepository(FakeRepository):
        def search(self, query: str, page: int = 1, limit: int = 20) -> SearchResult:
            return SearchResult(
                total=4000,
                page=page,
                pages=73,
                items=[replace(BLACK, ref=f"{BLACK.ref}-{page}-{index}") for index in range(limit)],
            )

    window = MainWindow(HugeRepository())
    window.show()
    app.processEvents()
    page = window._search
    page._query.setText("Бліч")
    page._search()
    assert pump_until(app, lambda: page._results.count() > 0)
    try:
        assert len(pager_chips(page)) == PAGER_CHIP_WINDOW

        page._go_to_page(40)
        app.processEvents()
        assert pump_until(app, lambda: page._page == 40)
        chips = pager_chips(page)
        labels = [chip.text() for chip in chips]
        # the active page sits in the middle of the window
        assert labels[0] == "1981 – 2035", labels
        assert chips[3].objectName() == "pagerChipActive"
    finally:
        window._runner.wait_for_done(10000)
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
        # Every feed row exposes a poster slot sourced from the API thumb URL.
        for key in ("updates", "schedule"):
            row = page._feed_rows[key].itemAt(0).widget()
            assert row.findChild(QLabel, "homeFeedPoster") is not None
        assert [thumb for thumb, _ in page._feed_poster_labels["updates"]] == [BLACK.thumb_url]
        assert [thumb for thumb, _ in page._feed_poster_labels["schedule"]] == [JACK.thumb_url]
        assert BLACK.ref in page._settings.values["home/recentTitles"]
    finally:
        page._runner.wait_for_done(10000)
        page.close()
        page.deleteLater()
        app.processEvents()


def test_a_feed_switched_off_in_settings_is_never_requested(app, isolated_settings):
    from PySide6.QtCore import QSettings

    from mikai_ui import feeds
    from mikai_ui.main import SearchPage

    QSettings(str(isolated_settings), QSettings.Format.IniFormat).setValue(
        feeds.SETTINGS_KEY, "ongoing,schedule"
    )
    calls: list[str] = []

    class PickyRepository(FakeRepository):
        def browse_popular(self, page: int = 1, limit: int = 18) -> SearchResult:
            calls.append("popular")
            return super().browse_popular(page=page, limit=limit)

        def get_updates(self, page: int = 1, limit: int = 12) -> UpdatesResult:
            calls.append("updates")
            return super().get_updates(page=page, limit=limit)

    repository = PickyRepository()
    page = SearchPage(Runner(), repository)
    page.show()
    try:
        assert pump_until(app, lambda: bool(page._carousel_widgets.get("ongoing")))
        assert "popular" not in calls, calls
        assert "updates" not in calls, calls
        assert page._feed_section("popular").isHidden()
        assert page._feed_section("updates").isHidden()
        assert page._feed_section("ongoing").isVisible()
        assert page._feed_section("schedule").isVisible()
    finally:
        page._runner.wait_for_done(10000)
        page.close()
        page.deleteLater()
        app.processEvents()


def test_settings_dialog_saves_feed_choices_and_the_page_applies_them(fake_window, app):
    from mikai_ui import feeds
    from mikai_ui.settings_dialog import SettingsDialog

    page = fake_window._search
    dialog = SettingsDialog(fake_window._repository, fake_window._runner)
    dialog._feed_boxes["popular"].setChecked(False)
    dialog._feed_boxes["schedule"].setChecked(False)
    dialog._save()

    assert feeds.enabled(dialog._settings) == {"ongoing", "updates"}
    calls: list[str] = []
    page._repository.browse_popular = lambda *a, **k: calls.append("popular") or SearchResult()
    page.refresh_feed_settings()
    app.processEvents()
    assert page._feed_section("popular").isHidden()
    assert "popular" not in calls


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
        page._carousel_widgets[key][0].poster.setPixmap(QPixmap())
        page._carousel_widgets[key][0].poster.setText("Постер недоступний")
    page._apply_carousel_thumbs({"img:black": PNG_1PX})
    assert all(
        page._carousel_widgets[key][0].poster.text() == ""
        and page._carousel_widgets[key][0].poster.pixmap() is not None
        and not page._carousel_widgets[key][0].poster.pixmap().isNull()
        for key in ("ongoing", "popular")
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
        window._runner.wait_for_done(10000)
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
    assert 4 <= narrow_columns <= 12, narrow_columns
    assert wide_columns > narrow_columns
    first_row_card = grid.layout().itemAt(0).widget()
    second_row_card = grid.layout().itemAt(wide_columns).widget()
    last_card = grid.layout().itemAt(len(grid._cards) - 1).widget()
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
    assert len(tab_chips(title)) == 24
    assert labels[0] == "Озвучення — Loud · 24 серій"


def test_clicking_episode_fetches_and_plays_it(fake_window, app, monkeypatch):
    """An episode click is the one moment qualities are fetched for that episode."""
    calls: list[str] = []

    def mock_streams(url):
        calls.append(url)
        return {720: f"{url}/720.m3u8"}

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)
    monkeypatch.setattr(
        "mikai_ui.title_page.prepare_runtime",
        lambda: (_ for _ in ()).throw(OSError("offline test")),
    )
    title = open_fake_title(fake_window, app)
    buttons = tab_chips(title)
    assert len(buttons) == 24
    expected = buttons[0].property("embed")
    QTest.mouseClick(buttons[0], Qt.MouseButton.LeftButton)
    app.processEvents()
    assert pump_until(app, lambda: len(calls) == 1), calls
    assert calls[0] == expected
    assert pump_until(app, lambda: len(title._player_dialogs) == 1)
    assert title._player_dialogs[-1].controller.state.quality == 720
    title.shutdown()
    app.processEvents()


class _FailingThenOkAdapter(FakeMpvAdapter):
    """Fails the first load so the dialog's re-extraction fallback is exercised."""

    def __init__(self, *_args, **_kwargs) -> None:
        super().__init__()

    def load(self, url: str) -> None:
        super().load(url)
        if len(self.loads) == 1:
            self.errorOccurred.emit("HTTP 403 forbidden")


def test_episode_qualities_are_fetched_once_and_cached(fake_window, app, monkeypatch):
    calls: list[str] = []

    def mock_streams(url):
        calls.append(url)
        return {720: f"{url}/720.m3u8"}

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)

    class _PlainAdapter(FakeMpvAdapter):
        def __init__(self, *_args, **_kwargs) -> None:
            super().__init__()

    # a working adapter, or the one-shot recovery fetch skews the call count
    monkeypatch.setattr("mikai_ui.title_page.prepare_runtime", lambda: Path("."))
    monkeypatch.setattr("mikai_ui.player_dialog.MPVAdapter", _PlainAdapter)
    title = open_fake_title(fake_window, app)
    button = tab_chips(title)[0]
    watched = button.property("embed")

    QTest.mouseClick(button, Qt.MouseButton.LeftButton)
    app.processEvents()
    assert pump_until(app, lambda: len(calls) == 1), calls
    # a second click reads the cache, not the network
    QTest.mouseClick(button, Qt.MouseButton.LeftButton)
    app.processEvents()
    assert pump_until(app, lambda: len(title._player_dialogs) == 2)
    assert calls.count(watched) == 1, calls

    title.shutdown()
    app.processEvents()


def test_no_source_click_is_cached_negatively(fake_window, app, monkeypatch):
    calls: list[str] = []

    def mock_streams(url):
        calls.append(url)
        raise streams.NoSourceError(f"moonanime has no video source for {url} (no_source)")

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)
    title = open_fake_title(fake_window, app)
    button = tab_chips(title)[0]
    watched = button.property("embed")

    QTest.mouseClick(button, Qt.MouseButton.LeftButton)
    app.processEvents()
    assert pump_until(app, lambda: len(calls) == 1), calls
    assert pump_until(app, lambda: "немає відео" in title._status.text())
    assert not button.isEnabled(), "a dead episode must look dead"

    # a second click reads the negative cache; repainting must not refetch either
    QTest.mouseClick(button, Qt.MouseButton.LeftButton)
    title._tabs.setCurrentIndex(1)
    app.processEvents()
    title._tabs.setCurrentIndex(0)
    app.processEvents()
    assert calls.count(watched) == 1, calls
    assert not title._tab_states[0]["visible"][0]["button"].isEnabled()

    title.shutdown()
    app.processEvents()


def test_failed_playback_reextracts_before_reporting_the_error(fake_window, app, monkeypatch):
    generations = {"n": 0}
    calls: list[str] = []

    def mock_streams(url):
        calls.append(url)
        generations["n"] += 1
        return {720: f"https://s.example/{generations['n']}/720.m3u8"}

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)
    monkeypatch.setattr("mikai_ui.title_page.prepare_runtime", lambda: Path("."))
    monkeypatch.setattr("mikai_ui.player_dialog.MPVAdapter", _FailingThenOkAdapter)
    title = open_fake_title(fake_window, app)
    button = tab_chips(title)[0]
    QTest.mouseClick(button, Qt.MouseButton.LeftButton)
    app.processEvents()
    assert pump_until(app, lambda: len(title._player_dialogs) == 1)
    dialog = title._player_dialogs[-1]
    assert pump_until(app, lambda: dialog.controller.adapter is not None)
    adapter = dialog.controller.adapter
    watched = button.property("embed")

    # The first load fails; the dialog re-extracts and retries before giving up
    stale_url = adapter.loads[0]
    assert pump_until(app, lambda: len(adapter.loads) == 2), adapter.loads
    assert adapter.loads[1] != stale_url, "player retried the expired URL"
    assert calls.count(watched) == 2, calls
    assert dialog.status_text == "Відео завантажується…", dialog.status_text
    assert title._quality_cache[watched] == {720: adapter.loads[1]}
    title.shutdown()
    app.processEvents()


def test_strict_player_tab_isolation(fake_window, app, monkeypatch):
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})
    title = open_fake_title(fake_window, app)
    tab_names = {title._tabs.tabText(i) for i in range(title._tabs.count())}
    assert tab_names == {"Ashdi", "MoonAnime", "Tortuga"}

    ashdi = tab_chips(title, 0)
    assert len(ashdi) == 24
    for button in ashdi:
        embed = button.property("embed")
        assert "ashdi.vip" in embed, embed
        assert "moon" not in embed and "tortuga" not in embed

    title._tabs.setCurrentIndex(2)
    app.processEvents()
    tortuga = tab_chips(title, 2)
    assert len(tortuga) >= 12
    for button in tortuga:
        assert "tortuga.tw" in button.property("embed")

    title._tabs.setCurrentIndex(1)
    app.processEvents()
    moon = tab_chips(title, 1)
    assert len(moon) >= 12
    for button in moon:
        assert "moonanime.art" in button.property("embed")

    # a 24-episode season is one page: the pager stays out of the way
    for state in title._tab_states:
        assert state["pager"].isHidden()
    title.shutdown()
    app.processEvents()


def test_release_pager_jumps_to_the_first_and_last_page(app, monkeypatch):
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})

    class LongReleaseRepository(FakeRepository):
        def get_player(self, ref: str) -> PlayerResult:
            return PlayerResult(releases=[_release("voice", "Loud", list(range(1, 301)))])

    window = MainWindow(LongReleaseRepository())
    window.show()
    try:
        window.open_title("7428", None)
        title = window._title
        assert pump_until(app, lambda: title._selector.count() == 1)
        state = title._tab_states[0]
        pager = state["pager"]
        assert pager.jump.maximum() == 3
        assert not pager.first_btn.isEnabled()
        assert not pager.prev_btn.isEnabled()
        assert pager.last_btn.isEnabled()

        pager.last_btn.click()
        app.processEvents()
        assert state["page"] == 2
        assert tab_chips(title)[0].text() == "281"
        assert not pager.last_btn.isEnabled()
        assert not pager.next_btn.isEnabled()
        assert pager.first_btn.isEnabled()

        pager.first_btn.click()
        app.processEvents()
        assert state["page"] == 0
        assert tab_chips(title)[0].text() == "1"
    finally:
        window._runner.wait_for_done(10000)
        window.close()
        window.deleteLater()
        app.processEvents()


def test_release_pager_jump_box_takes_a_typed_page(app, monkeypatch):
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})

    class LongReleaseRepository(FakeRepository):
        def get_player(self, ref: str) -> PlayerResult:
            return PlayerResult(releases=[_release("voice", "Loud", list(range(1, 301)))])

    window = MainWindow(LongReleaseRepository())
    window.show()
    try:
        window.open_title("7428", None)
        title = window._title
        assert pump_until(app, lambda: title._selector.count() == 1)
        state = title._tab_states[0]
        pager = state["pager"]
        assert pager.jump.maximum() == 3
        assert pager.jump.value() == 1

        pager.jump.setValue(2)
        app.processEvents()
        assert state["page"] == 1
        assert tab_chips(title)[0].text() == "141"
        assert pager.jump.value() == 2
    finally:
        window._runner.wait_for_done(10000)
        window.close()
        window.deleteLater()
        app.processEvents()


def test_release_pager_offers_range_chips_that_jump_pages(app, monkeypatch):
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})

    class LongReleaseRepository(FakeRepository):
        def get_player(self, ref: str) -> PlayerResult:
            return PlayerResult(releases=[_release("voice", "Loud", list(range(1, 301)))])

    window = MainWindow(LongReleaseRepository())
    window.show()
    try:
        window.open_title("7428", None)
        title = window._title
        assert pump_until(app, lambda: title._selector.count() == 1)
        state = title._tab_states[0]
        prev_btn, chips, next_btn = pagination_controls(title)
        assert [chip.text() for chip in chips] == ["1 – 140", "141 – 280", "281 – 300"]
        assert chips[0].objectName() == "pagerChipActive"

        chips[1].click()
        app.processEvents()
        assert state["page"] == 1
        assert tab_chips(title)[0].text() == "141"
        _, live_chips, _ = pagination_controls(title)
        assert live_chips[1].objectName() == "pagerChipActive"
    finally:
        window._runner.wait_for_done(10000)
        window.close()
        window.deleteLater()
        app.processEvents()


def test_track_picker_labels_never_carry_the_episode_count(fake_window, app, monkeypatch):
    """Regression: the track menu showed "· 12 серій" while episode 1 was playing,
    so the count read as the length of what was about to play."""
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})
    title = open_fake_title(fake_window, app)
    for variant in title._episode_variants("1"):
        assert "серій" not in variant["label"], variant["label"]
    # the release selector combo still shows the count
    selector_labels = [title._selector.itemText(i) for i in range(title._selector.count())]
    assert any("серій" in label for label in selector_labels), selector_labels
    title.shutdown()
    app.processEvents()


def test_opening_an_episode_hands_the_player_the_tab_playlist(fake_window, app, monkeypatch):
    """The player's step buttons and sidebar come from the release tab being watched."""
    calls: list[str] = []

    def mock_streams(url):
        calls.append(url)
        return {720: f"{url}/720.m3u8"}

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)
    monkeypatch.setattr(
        "mikai_ui.title_page.prepare_runtime",
        lambda: (_ for _ in ()).throw(OSError("offline test")),
    )
    title = open_fake_title(fake_window, app)
    buttons = tab_chips(title)
    # the sixth episode, so the player lands mid-list and both step buttons show
    QTest.mouseClick(buttons[5], Qt.MouseButton.LeftButton)
    app.processEvents()
    assert pump_until(app, lambda: len(title._player_dialogs) == 1)
    dialog = title._player_dialogs[-1]

    assert [item["label"] for item in dialog._playlist][:3] == ["Серія 1", "Серія 2", "Серія 3"]
    assert len(dialog._playlist) == 24
    assert dialog._episode_index == 5
    assert dialog._controls.prev_ep_button.isVisible()
    assert dialog._controls.next_ep_button.isVisible()
    title.shutdown()
    app.processEvents()


def test_release_chips_probe_one_episode_each_and_only_when_the_list_opens(
    fake_window, app, monkeypatch
):
    """One request per release, not per episode, and only once the list is
    actually opened: the chip is invisible until the popup is up."""
    from mikai_ui.release_quality import QUALITY_ROLE

    calls: list[str] = []

    def mock_streams(url):
        calls.append(url)
        return {480: "a", 720: "b", 1080: "c"}

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)
    title = open_fake_title(fake_window, app)
    assert title._selector.count() == 4
    assert calls == [], "rendering must not probe any release"

    title._selector.showPopup()
    app.processEvents()
    assert pump_until(app, lambda: len(calls) == 4), calls
    for index in range(title._selector.count()):
        assert title._selector.itemData(index, QUALITY_ROLE) == 1080, index

    title._selector.hidePopup()
    app.processEvents()
    title._selector.showPopup()
    app.processEvents()
    assert pump_until(app, lambda: True, timeout=1)
    app.processEvents()
    assert len(calls) == 4, "reopening must not re-probe"
    title.shutdown()
    app.processEvents()


def test_a_failed_release_probe_leaves_the_other_chips_intact(fake_window, app, monkeypatch):
    from mikai_ui.release_quality import QUALITY_ROLE

    def mock_streams(url):
        if "SubTeam" in url:
            raise RuntimeError("provider is down")
        return {720: "a", 1080: "b"}

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)
    title = open_fake_title(fake_window, app)
    title._selector.showPopup()
    app.processEvents()
    assert pump_until(app, lambda: title._selector.itemData(0, QUALITY_ROLE) == 1080), [
        title._selector.itemData(i, QUALITY_ROLE) for i in range(4)
    ]
    # the release that failed simply carries no chip
    assert title._selector.itemData(3, QUALITY_ROLE) in (None, 0)
    title.shutdown()
    app.processEvents()


def test_max_quality_ignores_unusable_streams():
    from mikai_ui.release_quality import max_quality

    assert max_quality({480: "u", 720: "u", 1080: "u"}) == 1080
    assert max_quality({}) == 0
    assert max_quality({720: ""}) == 0, "an empty stream URL is not a quality"
    assert max_quality({1080: None}) == 0


def test_a_release_numbered_from_110_labels_its_chips_with_real_numbers(
    fake_window, app, monkeypatch
):
    """A later cour can start at 110, so a chip reading "1 - 140" above a grid
    starting at 110 is a lie."""
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})

    class OffsetRepository(FakeRepository):
        def get_player(self, ref: str) -> PlayerResult:
            loud = _release("voice", "Loud", list(range(110, 400)))
            sub = Release(
                kind="sub",
                teams=[Team(id=2, slug="sundragon", name="SunDragon")],
                episodes=[
                    Episode(
                        number=n,
                        sources=[
                            Source(provider="moon", embed_url=f"https://moonanime.art/iframe/{n}/")
                        ],
                    )
                    for n in range(110, 250)
                ],
            )
            return PlayerResult(releases=[loud, sub])

    window = MainWindow(OffsetRepository())
    window.show()
    try:
        window.open_title("7428", None)
        title = window._title
        assert pump_until(app, lambda: title._selector.count() == 2)
        title._selector.setCurrentIndex(0)
        app.processEvents()
        state = title._tab_states[0]
        _, chips, _ = pagination_controls(title)
        labels = [c.text() for c in chips]
        assert labels[0] == "110 – 249", labels
        # the grid underneath really does start at 110
        assert tab_chips(title)[0].text() == "110"
    finally:
        window._runner.wait_for_done(10000)
        window.close()
        window.deleteLater()
        app.processEvents()


def test_subtitle_tracks_appear_in_the_mic_menu(fake_window, app, monkeypatch):
    """Episode.number is an int; the variant matcher compared it to a str and
    silently dropped every subtitle track."""
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})

    class SubOnlyRepository(FakeRepository):
        def get_player(self, ref: str) -> PlayerResult:
            sub = Release(
                kind="sub",
                teams=[Team(id=2, slug="sundragon", name="SunDragon")],
                episodes=[
                    Episode(
                        number=n,
                        sources=[
                            Source(provider="moon", embed_url=f"https://moonanime.art/iframe/{n}/")
                        ],
                    )
                    for n in range(1, 13)
                ],
            )
            return PlayerResult(releases=[sub])

    window = MainWindow(SubOnlyRepository())
    window.show()
    try:
        window.open_title("7428", None)
        title = window._title
        assert pump_until(app, lambda: title._selector.count() == 1)
        variants = title._episode_variants("1")
        assert len(variants) == 1, [v["label"] for v in variants]
        assert variants[0]["kind"] == "sub", variants[0]
        assert not variants[0]["display"].startswith("Субтитри"), variants[0]
    finally:
        window._runner.wait_for_done(10000)
        window.close()
        window.deleteLater()
        app.processEvents()


def test_episode_variants_cover_every_release_track(fake_window, app, monkeypatch):
    """The mic menu offers every dub/sub track carrying the episode, deduped."""
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})
    title = open_fake_title(fake_window, app)
    variants = title._episode_variants("1")
    embeds = [v["embed"] for v in variants]
    assert len(embeds) == len(set(embeds)), "the same track must not appear twice"
    # four releases, each carried by three providers
    assert len(variants) == 12, [v["label"] for v in variants]
    assert any(v["track"] == "Ashdi" for v in variants)
    assert any(v["kind"] == "sub" for v in variants)
    title.shutdown()
    app.processEvents()


def test_track_menu_groups_by_kind_and_picks_a_track(app):
    """Dubs and subs are listed under their own heading, each row badged with the
    quality that track serves, and picking one reports it."""
    from mikai_ui.player_controls import PlayerControlsWidget

    controls = PlayerControlsWidget()
    controls.set_variants(
        [
            {
                "kind": "voice",
                "display": "Loud",
                "track": "Ashdi",
                "label": "Озвучення — Loud · Ashdi",
                "embed": "a",
                "quality": 1080,
            }
        ],
        "a",
    )
    assert not controls.variant_button.isVisibleTo(controls), "one track needs no picker"

    controls.set_variants(
        [
            {
                "kind": "voice",
                "display": "Loud",
                "track": "Ashdi",
                "label": "Озвучення — Loud · Ashdi",
                "embed": "a",
                "quality": 1080,
            },
            {
                "kind": "voice",
                "display": "Mid",
                "track": "MoonAnime",
                "label": "Озвучення — Mid · MoonAnime",
                "embed": "b",
                "quality": 0,
            },
            {
                "kind": "sub",
                "display": "SubTeam",
                "track": "Ashdi",
                "label": "Субтитри — SubTeam · Ashdi",
                "embed": "s",
                "quality": 480,
            },
        ],
        "a",
    )
    assert controls.variant_button.isVisibleTo(controls)

    menu = controls._variant_menu()
    rows = [a for a in menu.actions() if not a.isSeparator() and a.text()]
    # two heading sections, then the three tracks, in voice-then-sub order
    assert [a.text() for a in rows] == [
        "Loud · Ashdi · 1080p",
        "Mid · MoonAnime",  # no quality yet: bare, never a fake one
        "SubTeam · Ashdi · 480p",
    ]
    assert menu.actions()[0].text() == "Озвучення"
    assert [a.isChecked() for a in rows] == [True, False, False]
    assert all(not a.icon().isNull() for a in rows), "every track carries its kind icon"

    picked = []
    controls.variantRequested.connect(lambda embed, label: picked.append((embed, label)))
    rows[2].trigger()
    assert picked == [("s", "Субтитри — SubTeam · Ashdi")], picked
    controls.deleteLater()
    menu.deleteLater()


def test_track_rows_do_not_repeat_the_kind_the_heading_already_says(fake_window, app, monkeypatch):
    """Regression: the row text carried "Озвучення -" while sitting directly under
    an "Озвучення" heading, so every row said it twice."""
    from mikai_backend import Episode, Release, Source, Team

    def rel(kind, team, provider, host):
        return Release(
            kind=kind,
            teams=[Team(name=team)],
            episodes=[
                Episode(
                    number=1,
                    label="1",
                    sources=[Source(provider=provider, embed_url=f"https://{host}/1")],
                )
            ],
        )

    from mikai_ui.player_controls import PlayerControlsWidget

    variants = [
        {
            "kind": "voice",
            "kind_label": "Озвучення",
            "display": "QTV+Kaizoku",
            "track": "Ashdi",
            "label": "Озвучення — QTV+Kaizoku · Ashdi",
            "embed": "a",
            "quality": 480,
        },
        {
            "kind": "sub",
            "kind_label": "Субтитри",
            "display": "AniSky",
            "track": "MoonAnime",
            "label": "Субтитри — AniSky · MoonAnime",
            "embed": "b",
            "quality": 1080,
        },
    ]
    controls = PlayerControlsWidget()
    controls.set_variants(variants, "a")
    texts = [a.text() for a in controls._variant_menu().actions() if not a.isSeparator()]
    assert texts == [
        "QTV+Kaizoku · Ashdi · 480p",
        "AniSky · MoonAnime · 1080p",
    ], texts
    controls.deleteLater()

    # and the title page must hand over a label without the kind baked in
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})
    title = open_fake_title(fake_window, app)
    for variant in title._episode_variants("1"):
        assert not variant["display"].startswith("Озвучення"), variant["display"]
        assert not variant["display"].startswith("Субтитри"), variant["display"]
        assert "серій" not in variant["display"], variant["display"]
    title.shutdown()
    app.processEvents()


def test_opening_the_track_menu_probes_only_unknown_qualities(app):
    """Lazy: a track nobody looks at never costs a request."""
    from mikai_ui.player_controls import PlayerControlsWidget

    controls = PlayerControlsWidget()
    controls.set_variants(
        [
            {"kind": "voice", "label": "Озвучення — Loud · Ashdi", "embed": "a", "quality": 1080},
            {
                "kind": "sub",
                "display": "Sub",
                "track": "Ashdi",
                "label": "Субтитри — Sub · Ashdi",
                "embed": "s",
                "quality": 0,
            },
        ],
        "a",
    )
    assert controls.pending_track_probes() == ["s"], "the probed one must not re-fetch"

    controls.set_variants(
        [
            {
                "kind": "voice",
                "display": "Loud",
                "track": "Ashdi",
                "label": "Озвучення — Loud · Ashdi",
                "embed": "a",
                "quality": 0,
            }
        ],
        "a",
    )
    assert controls.pending_track_probes() == ["a"]
    controls.deleteLater()


def test_a_late_track_probe_badges_the_open_row(app):
    """The result must land in the open menu, not rebuild it under the pointer."""
    from mikai_ui.player_controls import PlayerControlsWidget

    controls = PlayerControlsWidget()
    controls.set_variants(
        [
            {
                "kind": "sub",
                "display": "Sub",
                "track": "Ashdi",
                "label": "Субтитри — Sub · Ashdi",
                "embed": "s",
                "quality": 0,
            }
        ],
        "a",
    )
    menu = controls._variant_menu()
    controls._open_menu = menu
    row_action = next(a for a in menu.actions() if not a.isSeparator())
    controls._menu_actions = [(row_action, controls._variants[0])]
    row = controls._menu_actions[0][0]
    assert row.text() == "Sub · Ashdi"

    controls.set_variants(
        [
            {
                "kind": "sub",
                "display": "Sub",
                "track": "Ashdi",
                "label": "Субтитри — Sub · Ashdi",
                "embed": "s",
                "quality": 720,
            }
        ],
        "a",
    )
    assert row.text() == "Sub · Ashdi · 720p", "the open row must update"
    controls.deleteLater()
    menu.deleteLater()


def test_variant_switch_swaps_stream_and_keeps_position(fake_window, app, monkeypatch):
    """Pick another track in the player: one fetch, new URL, same position."""
    calls: list[str] = []

    def mock_streams(url):
        calls.append(url)
        return {720: f"{url}/720.m3u8"}

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)

    class _PlainAdapter(FakeMpvAdapter):
        def __init__(self, *_args, **_kwargs) -> None:
            super().__init__()

    monkeypatch.setattr("mikai_ui.title_page.prepare_runtime", lambda: Path("."))
    monkeypatch.setattr("mikai_ui.player_dialog.MPVAdapter", _PlainAdapter)
    title = open_fake_title(fake_window, app)
    button = tab_chips(title)[0]
    QTest.mouseClick(button, Qt.MouseButton.LeftButton)
    app.processEvents()
    assert pump_until(app, lambda: len(title._player_dialogs) == 1)
    dialog = title._player_dialogs[-1]
    assert pump_until(app, lambda: dialog.controller.adapter is not None)
    adapter = dialog.controller.adapter
    adapter.durationChanged.emit(1200.0)
    adapter.positionChanged.emit(140.0)
    adapter.mediaLoaded.emit()
    app.processEvents()

    variants = title._episode_variants(button._entry["number"])
    other = next(v for v in variants if v["embed"] != button._entry["embed"])
    fetched_before = len(calls)
    dialog._on_variant_requested(other["embed"], other["label"])
    app.processEvents()
    assert pump_until(app, lambda: len(adapter.loads) == 2), adapter.loads

    assert calls[fetched_before:] == [other["embed"]], calls
    assert adapter.loads[-1] == f"{other['embed']}/720.m3u8"
    assert dialog._page_entry["embed"] == other["embed"]
    adapter.mediaLoaded.emit()
    app.processEvents()
    assert adapter.seeks[-1] == 140.0, adapter.seeks
    title.shutdown()
    app.processEvents()


def test_no_quality_extraction_happens_until_an_episode_is_played(fake_window, app, monkeypatch):
    """The old design fetched qualities for every visible episode on render."""
    calls: list[str] = []

    def mock_streams(url):
        calls.append(url)
        return {720: f"{url}/720.m3u8"}

    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", mock_streams)
    monkeypatch.setattr(
        "mikai_ui.title_page.prepare_runtime",
        lambda: (_ for _ in ()).throw(OSError("offline test")),
    )
    title = open_fake_title(fake_window, app)
    QTest.qWait(150)
    app.processEvents()
    assert calls == [], "rendering must not cost a single quality fetch"

    buttons = tab_chips(title)
    assert len(buttons) == 24
    QTest.mouseClick(buttons[7], Qt.MouseButton.LeftButton)
    app.processEvents()
    assert pump_until(app, lambda: len(calls) == 1), calls
    assert calls[0] == buttons[7].property("embed")
    title.shutdown()
    app.processEvents()


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
