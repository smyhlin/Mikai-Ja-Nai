import pytest
import requests
from conftest import episode_button, pump_until, tab_chips
from PySide6.QtCore import Qt
from PySide6.QtGui import QGuiApplication
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from mikai_backend import streams


@pytest.fixture(autouse=True)
def _no_stream_network(monkeypatch):
    monkeypatch.setattr("mikai_ui.title_page.streams.get_quality_streams", lambda url: {})


@pytest.fixture(autouse=True)
def _no_home_card_details(monkeypatch):
    """These tests open title pages, not home cards, but every window fixture builds the
    home page too — and its one-detail-per-card fan-out is ~54 requests per window, which
    outruns the API key's 300/min and parks every client on 429s. The card path is
    covered hermetically in test_units."""
    monkeypatch.setattr("mikai_ui.main.SearchPage._request_card_details", lambda self, cards: None)


from mikai_ui.title_page import TitlePage

REF = "7428"


def open_title_for(win: QApplication, app: QApplication, query: str) -> TitlePage:
    page = win._search
    page._query.setText(query)
    page._search()
    assert pump_until(
        app, lambda: page._results.count() > 0 or page._status.text() not in ("", "Пошук…")
    ), f"search stuck: {page._status.text()!r}"
    assert page._results.count() > 0, f"no results: {page._status.text()!r}"
    data = page._results.item(0).data(Qt.ItemDataRole.UserRole)
    win.open_title(data["ref"], data["poster"])
    title = win._title
    assert pump_until(app, lambda: title._heading.text() != ""), "title load stuck"
    return title


def release_index(title: TitlePage, kind: str, team: str) -> int:
    for index in range(title._selector.count()):
        label = title._selector.itemText(index)
        if label.startswith(kind) and team in label:
            return index
    raise AssertionError(f"release not found: {kind} / {team} in {title._releases}")


def test_search_returns_black_torch_first(window, app):
    page = window._search
    page._query.setText("Чорний смолоскип")
    page._search()
    assert pump_until(app, lambda: page._results.count() > 0)
    data = page._results.item(0).data(Qt.ItemDataRole.UserRole)
    assert data["ref"] == REF
    assert "Чорний смолоскип" in page._results.item(0).text()


def test_english_search_returns_black_torch_first(window, app):
    page = window._search
    page._query.setText("BLACK TORCH")
    page._search()
    assert pump_until(app, lambda: page._results.count() > 0)
    data = page._results.item(0).data(Qt.ItemDataRole.UserRole)
    assert data["ref"] == REF


def test_title_page_shows_description_and_cover(window, app):
    title = open_title_for(window, app, "Чорний смолоскип")
    assert title._heading.text() == "Чорний смолоскип"
    assert "BLACK TORCH" in title._subtitle.text()
    assert len(title._description.text()) > 100
    assert pump_until(
        app, lambda: title._cover.pixmap() is not None and not title._cover.pixmap().isNull()
    ), "cover did not load"


def test_title_page_has_four_voice_and_three_sub_releases(window, app):
    title = open_title_for(window, app, "Чорний смолоскип")
    assert pump_until(app, lambda: title._selector.count() == 7)
    labels = [title._selector.itemText(i) for i in range(title._selector.count())]
    voices = [label for label in labels if label.startswith("Озвучення")]
    subs = [label for label in labels if label.startswith("Субтитри")]
    assert len(voices) == 4, labels
    assert len(subs) == 3, labels
    joined = " | ".join(labels)
    for team in ("Inari", "SunDragon", "Рідний Голос", "Didko", "Цукіхана"):
        assert team in joined, joined


def test_twelve_episode_release_lists_all_urls_from_moon_and_ashdi(window, app):
    title = open_title_for(window, app, "Чорний смолоскип")
    assert pump_until(app, lambda: title._selector.count() == 7)
    title._selector.setCurrentIndex(release_index(title, "Озвучення", "Рідний Голос"))
    app.processEvents()
    tab_labels = {title._tabs.tabText(i) for i in range(title._tabs.count())}
    assert {"Ashdi", "MoonAnime"} <= tab_labels, tab_labels
    all_numbers: set[str] = set()
    for tab in range(title._tabs.count()):
        numbers = {
            episode_button(chip).text().removeprefix("Серія ") for chip in tab_chips(title, tab)
        }
        assert "12" in numbers, (title._tabs.tabText(tab), sorted(numbers))
        all_numbers |= numbers
    assert {str(i) for i in range(1, 13)} <= all_numbers, sorted(all_numbers)


def test_clicking_episode_copies_its_url(window, app):
    title = open_title_for(window, app, "Чорний смолоскип")
    assert pump_until(app, lambda: title._selector.count() == 7)
    title._selector.setCurrentIndex(release_index(title, "Субтитри", "Рідний Голос"))
    app.processEvents()
    assert title._tabs.count() > 0
    chips = tab_chips(title)
    assert chips
    expected = chips[0].property("embed")
    assert expected.startswith("http"), expected
    QTest.mouseClick(episode_button(chips[0]), Qt.MouseButton.LeftButton)
    app.processEvents()
    assert title._url.text() == expected
    assert QGuiApplication.clipboard().text() == expected
    assert "скопійовано" in title._status.text().lower()
    assert pump_until(app, lambda: title._status.text() == "", timeout=5)


def test_copy_all_copies_active_player_tab(window, app):
    title = open_title_for(window, app, "Чорний смолоскип")
    assert pump_until(app, lambda: title._selector.count() == 7)
    title._selector.setCurrentIndex(release_index(title, "Субтитри", "Рідний Голос"))
    app.processEvents()
    chips = tab_chips(title)
    assert chips
    title._copy_all.click()
    app.processEvents()
    copied = [line for line in QGuiApplication.clipboard().text().splitlines() if line]
    assert len(copied) == len(chips)


def test_search_results_show_cover_thumbnails(window, app):
    page = window._search
    page._query.setText("Чорний смолоскип")
    page._search()
    assert pump_until(app, lambda: page._results.count() > 0)
    first = page._results.item(0)
    assert pump_until(app, lambda: not first.icon().isNull()), "thumbnail did not load"


def test_format_tabs_filter_results_by_format(window, app):
    page = window._search
    page._query.setText("Чорний смолоскип")
    page._search()
    assert pump_until(app, lambda: page._results.count() > 0)
    bar = page._format_bar
    assert not bar.isHidden(), "format bar should be visible for mixed formats"
    labels = [bar.tabText(i) for i in range(bar.count())]
    assert labels[0].startswith("Усі"), labels
    for name in ("TV", "OVA", "MOVIE"):
        assert any(label.startswith(name) for label in labels), labels
    ova_index = next(i for i, label in enumerate(labels) if label.startswith("OVA"))
    bar.setCurrentIndex(ova_index)
    app.processEvents()
    rows = [page._results.item(r).text() for r in range(page._results.count())]
    assert rows, "no OVA rows"
    assert all(row.endswith("OVA") for row in rows), rows
    all_index = next(i for i, label in enumerate(labels) if label.startswith("Усі"))
    bar.setCurrentIndex(all_index)
    app.processEvents()
    assert page._results.count() == len(page._entries)
    assert page._results.item(0).text().startswith("Чорний смолоскип")


def test_live_stream_extraction_acceptance():
    session = requests.Session()
    cases = (
        ("https://ashdi.vip/vod/268104", streams.ASHDI_H),
        ("https://ashdi.vip/vod/113749", streams.ASHDI_H),
        ("https://tortuga.tw/vod/98249", streams.TORTUGA_H),
        ("https://moonanime.art/iframe/n7p0wo428uufidanqjswp1u8/", streams.MEDIA_H),
    )
    for page_url, master_headers in cases:
        site = streams.detect_site(page_url)
        master = streams.extract_master(session, page_url)
        assert ".m3u8" in master, (site, master[:120])
        variants = streams.parse_variants(
            streams.fetch_text(session, master, master_headers), master
        )
        heights = {variant["height"] for variant in variants}
        assert {480, 720, 1080} <= heights, (site, heights)
        quality_url = streams.get_quality_url(variants, 720)
        segments = streams.list_segments(session, quality_url, master_headers)
        assert len(segments) >= 5, (site, len(segments))
        assert segments[0].startswith("http"), (site, segments[0])
        first = session.get(segments[0], headers=master_headers, timeout=20)
        first.raise_for_status()
        # Segment duration and bitrate vary between live providers. MPEG-TS is
        # packetized in 188-byte units, so validate structure rather than size.
        assert len(first.content) >= 188 and len(first.content) % 188 == 0, (
            site,
            len(first.content),
        )
        assert first.content[:1] == b"G", (site, first.content[:16])


def test_releases_sorted_and_default_is_most_voiced_dub(window, app):
    title = open_title_for(window, app, "Чорний смолоскип")
    assert pump_until(app, lambda: title._selector.count() == 7)
    labels = [title._selector.itemText(i) for i in range(title._selector.count())]
    kinds = ["voice" if label.startswith("Озвучення") else "sub" for label in labels]
    assert kinds == ["voice"] * 4 + ["sub"] * 3, labels
    counts = [int(label.rsplit("·", 1)[-1].split()[0]) for label in labels]
    assert counts[:4] == sorted(counts[:4], reverse=True), labels
    assert counts[4:] == sorted(counts[4:], reverse=True), labels
    assert title._selector.currentIndex() == 0
    assert labels[0].startswith("Озвучення"), labels
    assert counts[0] == max(counts[:4]), labels


def test_clicking_result_opens_title_page(window, app):
    page = window._search
    page._query.setText("Чорний смолоскип")
    page._search()
    assert pump_until(app, lambda: page._results.count() > 0)
    item = page._results.item(0)
    QTest.mouseClick(
        page._results.viewport(),
        Qt.MouseButton.LeftButton,
        pos=page._results.visualItemRect(item).center(),
    )
    app.processEvents()
    assert window._stack.currentWidget() is window._title
    assert pump_until(app, lambda: window._title._heading.text() != "")
    assert window._title._heading.text() == "Чорний смолоскип"


def test_back_returns_to_search(window, app):
    title = open_title_for(window, app, "Чорний смолоскип")
    title.back_requested.emit()
    app.processEvents()
    assert window._stack.currentWidget() is window._search
    assert window._title is None
