from __future__ import annotations

import logging
import json
import sys
from dataclasses import replace
from datetime import datetime

import shiboken6
from PySide6.QtCore import QSettings, QSize, Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QDesktopServices, QGuiApplication, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QPushButton,
    QStackedWidget,
    QScrollArea,
    QTabBar,
    QVBoxLayout,
    QWidget,
)

from mikai_backend import (
    AiringEntry,
    AnimeDetail,
    AnimeRepository,
    AnimeSummary,
    anime_page_url,
    ImageVariant,
    MikaiClient,
    PosterSet,
    ScheduleResult,
    SearchResult,
    Titles,
    UpdatesResult,
)
from mikai_ui import feeds
from mikai_ui.title_card import TitleCard
from mikai_ui.rate_limit_label import RateLimitLabel
from mikai_ui.settings_dialog import SettingsDialog, make_gear_icon
from mikai_ui.theme import FG, build_stylesheet
from mikai_ui.theme.metrics import SEARCH_POSTER_HEIGHT, SEARCH_POSTER_WIDTH
from mikai_ui.theme.metrics import (
    HOME_CARD_HEIGHT,
    HOME_CARD_WIDTH,
    HOME_CAROUSEL_SCROLL_STEP,
    HOME_FEED_POSTER_HEIGHT,
    HOME_FEED_POSTER_WIDTH,
)
from mikai_ui.title_page import TitlePage
from mikai_ui.workers import Runner

log = logging.getLogger(__name__)


def _is_remote_url(value: str | None) -> bool:
    return bool(value) and value.startswith(("http://", "https://"))


def _poster_key(anime: AnimeSummary) -> str | None:
    return anime.thumb_url or anime.poster_url


class SearchPage(QWidget):
    open_requested = Signal(str, object)
    settings_requested = Signal()

    def __init__(
        self, runner: Runner, repository: AnimeRepository, parent: QWidget | None = None
    ) -> None:
        super().__init__(parent)
        self._runner = runner
        self._repository = repository
        self._entries: list[AnimeSummary] = []
        self._request_seq = 0
        self._browse_mode = True
        self._current_query = ""
        self._page = 1
        self._pages = 1
        self._page_size = 24
        self._thumb_cache: dict[str, bytes] = {}
        self._feed_poster_labels: dict[str, list[tuple[str, QLabel]]] = {}
        self._icons: dict[str, QPixmap] = {}
        self._carousel_widgets: dict[str, list[TitleCard]] = {}
        self._carousel_entries: dict[str, list[AnimeSummary]] = {}
        self._poster_lookups: set[str] = set()
        self._detail_lookups: set[str] = set()
        self._card_details: dict[str, AnimeDetail] = {}
        self._healed_posters: dict[str, str] = {}
        self._heal_timer = QTimer(self)
        self._heal_timer.setSingleShot(True)
        self._heal_timer.setInterval(200)
        self._heal_timer.timeout.connect(self._flush_recent_posters)
        self._feed_seq = {
            "ongoing": 0,
            "popular": 0,
            "updates": 0,
            "schedule": 0,
        }
        self._carousel_sections: dict[str, QWidget] = {}
        self._feed_sections: dict[str, QWidget] = {}
        self._feed_rows: dict[str, QVBoxLayout] = {}
        self._settings = QSettings("Mikai", "Mikai Wrapper")
        self._enabled_feeds = feeds.enabled(self._settings)
        self._recent_items = self._read_recent_titles()

        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        top_row = QHBoxLayout()
        heading = QLabel("Mikai")
        heading.setObjectName("heading")
        top_row.addWidget(heading)
        top_row.addStretch()

        self._rate_label = RateLimitLabel(repository)
        top_row.addWidget(self._rate_label)

        self._settings_btn = QPushButton()
        self._settings_btn.setObjectName("settingsChip")
        self._settings_btn.setIcon(make_gear_icon(18, FG))
        self._settings_btn.setIconSize(QSize(18, 18))
        self._settings_btn.setToolTip("Налаштування токена API")
        self._settings_btn.clicked.connect(self.settings_requested)
        top_row.addWidget(self._settings_btn)

        root.addLayout(top_row)

        search_row = QHBoxLayout()
        self._query = QLineEdit()
        self._query.setPlaceholderText("Пошук за назвою — Enter або кнопка")
        self._query.setClearButtonEnabled(True)
        self._query.returnPressed.connect(self._search)
        button = QPushButton("Пошук")
        button.setObjectName("primary")
        button.clicked.connect(self._search)
        search_row.addWidget(self._query)
        search_row.addWidget(button)
        root.addLayout(search_row)

        self._results_heading = QLabel("Зараз виходять · оновлені найсвіжішими серіями")
        self._results_heading.setObjectName("sectionHeading")
        root.addWidget(self._results_heading)

        self._status = QLabel("")
        self._status.setObjectName("status")
        root.addWidget(self._status)

        self._discovery = QWidget()
        discovery_layout = QVBoxLayout(self._discovery)
        discovery_layout.setContentsMargins(0, 0, 0, 0)
        discovery_layout.setSpacing(14)
        self._recent_carousel = self._build_carousel("Нещодавно відкриті", "recent")
        self._recent_carousel.setVisible(bool(self._recent_items))
        self._ongoing_carousel = self._build_carousel("Зараз виходять", "ongoing")
        self._popular_carousel = self._build_carousel("Популярні на Mikai", "popular")
        self._updates_section = self._build_feed_section("Останні оновлення", "updates")
        self._schedule_section = self._build_feed_section("Розклад виходу", "schedule")
        discovery_layout.addWidget(self._recent_carousel)
        discovery_layout.addWidget(self._ongoing_carousel)
        discovery_layout.addWidget(self._popular_carousel)
        discovery_layout.addWidget(self._updates_section)
        discovery_layout.addWidget(self._schedule_section)
        for key in feeds.ALL_FEEDS:
            self._feed_section(key).setVisible(key in self._enabled_feeds)
        discovery_layout.addStretch()
        self._discovery_scroll = QScrollArea()
        self._discovery_scroll.setObjectName("discoveryScroll")
        self._discovery_scroll.setWidgetResizable(True)
        self._discovery_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._discovery_scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        self._discovery_scroll.setWidget(self._discovery)
        root.addWidget(self._discovery_scroll, stretch=1)

        self._format_bar = QTabBar()
        self._format_bar.setExpanding(False)
        self._format_bar.setDrawBase(False)
        self._format_bar.hide()
        self._format_bar.currentChanged.connect(self._apply_format_filter)
        root.addWidget(self._format_bar)

        self._results = QListWidget()
        self._results.setIconSize(QSize(SEARCH_POSTER_WIDTH, SEARCH_POSTER_HEIGHT))
        self._results.setTextElideMode(Qt.TextElideMode.ElideRight)
        self._results.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._results.itemClicked.connect(self._open)
        self._results.itemActivated.connect(self._open)
        self._results.setContextMenuPolicy(Qt.CustomContextMenu)
        self._results.customContextMenuRequested.connect(self._results_context_menu)
        root.addWidget(self._results, stretch=1)
        self._results.hide()

        self._pager = QWidget()
        pager_row = QHBoxLayout(self._pager)
        pager_row.setContentsMargins(0, 0, 0, 0)
        self._previous = QPushButton("← Попередня")
        self._previous.clicked.connect(lambda: self._change_page(-1))
        self._page_label = QLabel("")
        self._page_label.setObjectName("muted")
        self._page_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._next = QPushButton("Наступна →")
        self._next.clicked.connect(lambda: self._change_page(1))
        pager_row.addStretch()
        pager_row.addWidget(self._previous)
        pager_row.addWidget(self._page_label)
        pager_row.addWidget(self._next)
        pager_row.addStretch()
        self._pager.hide()
        root.addWidget(self._pager)

        QTimer.singleShot(0, self._load_ongoing)
        if self._recent_items:
            self._show_carousel("recent", SearchResult(items=self._recent_items))

    def _feed_section(self, key: str) -> QWidget:
        section = self._carousel_sections.get(key) or self._feed_sections[key]
        return section

    def _build_carousel(self, heading: str, key: str) -> QWidget:
        section = QWidget()
        layout = QVBoxLayout(section)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)
        header = QHBoxLayout()
        title = QLabel(heading)
        title.setObjectName("sectionHeading")
        header.addWidget(title)
        header.addStretch()
        previous, next_button = QPushButton("←"), QPushButton("→")
        for button in (previous, next_button):
            button.setObjectName("carouselControl")
            header.addWidget(button)
        layout.addLayout(header)

        state_row = QHBoxLayout()
        state = QLabel("Добірка ще не завантажена")
        state.setObjectName("muted")
        state.setWordWrap(True)
        retry = QPushButton("Повторити")
        retry.setObjectName("secondary")
        retry.clicked.connect(lambda _checked=False, feed=key: self._request_recommendation(feed))
        retry.hide()
        state_row.addWidget(state, stretch=1)
        state_row.addWidget(retry)
        layout.addLayout(state_row)

        scroll = QScrollArea()
        scroll.setObjectName("animeCarousel")
        scroll.setWidgetResizable(False)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setFixedHeight(HOME_CARD_HEIGHT + 12)
        track = QWidget()
        track.setObjectName("animeCarouselTrack")
        cards = QHBoxLayout(track)
        cards.setContentsMargins(2, 2, 2, 2)
        cards.setSpacing(10)
        scroll.setWidget(track)
        layout.addWidget(scroll)
        bar = scroll.horizontalScrollBar()
        previous.clicked.connect(lambda: bar.setValue(bar.value() - HOME_CAROUSEL_SCROLL_STEP))
        next_button.clicked.connect(lambda: bar.setValue(bar.value() + HOME_CAROUSEL_SCROLL_STEP))
        previous.setEnabled(False)
        bar.rangeChanged.connect(
            lambda minimum, maximum, left=previous, right=next_button, current=bar: (
                left.setEnabled(current.value() > minimum),
                right.setEnabled(current.value() < maximum),
            )
        )
        bar.valueChanged.connect(
            lambda value, left=previous, right=next_button, current=bar: (
                left.setEnabled(value > current.minimum()),
                right.setEnabled(value < current.maximum()),
            )
        )
        section._carousel_cards = cards
        section._carousel_scroll = scroll
        section._carousel_state = state
        section._carousel_retry = retry
        scroll.hide()
        self._carousel_sections[key] = section
        return section

    def _build_feed_section(self, heading: str, key: str) -> QWidget:
        section = QWidget()
        layout = QVBoxLayout(section)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)
        header = QHBoxLayout()
        title = QLabel(heading)
        title.setObjectName("sectionHeading")
        header.addWidget(title)
        header.addStretch()
        retry = QPushButton("Оновити")
        retry.setObjectName("textAction")
        retry.clicked.connect(
            lambda _checked=False, feed=key: (
                self._load_updates() if feed == "updates" else self._load_schedule()
            )
        )
        header.addWidget(retry)
        layout.addLayout(header)

        state = QLabel("Завантаження…")
        state.setObjectName("muted")
        layout.addWidget(state)
        rows_host = QWidget()
        rows = QVBoxLayout(rows_host)
        rows.setContentsMargins(0, 0, 0, 0)
        rows.setSpacing(4)
        rows_host.hide()
        layout.addWidget(rows_host)

        section._feed_state = state
        section._feed_retry = retry
        section._feed_rows_host = rows_host
        self._feed_sections[key] = section
        self._feed_rows[key] = rows
        return section

    def _read_recent_titles(self) -> list[AnimeSummary]:
        try:
            records = json.loads(self._settings.value("home/recentTitles", "[]", str))
        except TypeError, ValueError:
            return []
        items = []
        for record in records if isinstance(records, list) else []:
            if not isinstance(record, dict) or not record.get("ref"):
                continue
            poster = str(record.get("poster") or "")
            items.append(
                AnimeSummary(
                    ref=str(record["ref"]),
                    titles=Titles(ua=str(record.get("title") or "")),
                    poster=(
                        PosterSet(big=ImageVariant(webp=poster))
                        if _is_remote_url(poster)
                        else PosterSet()
                    ),
                )
            )
        return items[:12]

    def record_recent_title(self, ref: str, title: str, poster: str) -> None:
        if not ref:
            return
        self._recent_items = [item for item in self._recent_items if item.ref != ref]
        item = AnimeSummary(
            ref=ref,
            titles=Titles(ua=title),
            poster=(
                PosterSet(big=ImageVariant(webp=poster)) if _is_remote_url(poster) else PosterSet()
            ),
        )
        self._recent_items.insert(0, item)
        self._recent_items = self._recent_items[:12]
        self._persist_recent_titles()
        self._recent_carousel.show()
        self._show_carousel("recent", SearchResult(items=self._recent_items))

    def _show_carousel(self, key: str, result: SearchResult) -> None:
        section = self._carousel_sections[key]
        section.show()
        section._carousel_retry.hide()
        cards = section._carousel_cards
        self._carousel_entries[key] = result.items[:18]
        if not result.items:
            section._carousel_state.setText("Наразі немає тайтлів у цій добірці.")
            section._carousel_scroll.hide()
            return
        section._carousel_state.setText(f"{len(result.items[:18])} тайтлів")
        while cards.count():
            item = cards.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        widgets: list[TitleCard] = []
        for anime in result.items[:18]:
            card = TitleCard(anime)
            card.openRequested.connect(self.open_requested)
            card.menuRequested.connect(self._anime_card_menu)
            cards.addWidget(card)
            widgets.append(card)
            if card.image_key and card.image_key in self._thumb_cache:
                pixmap = QPixmap()
                if pixmap.loadFromData(self._thumb_cache[card.image_key]) and not pixmap.isNull():
                    card.apply_pixmap(pixmap)
        track = section._carousel_scroll.widget()
        cards.activate()
        margins = cards.contentsMargins()
        track_width = (
            margins.left()
            + margins.right()
            + len(result.items[:18]) * HOME_CARD_WIDTH
            + max(0, len(result.items[:18]) - 1) * cards.spacing()
        )
        track.setFixedSize(track_width, HOME_CARD_HEIGHT + 4)
        section._carousel_scroll.widget().show()
        section._carousel_scroll.show()
        self._carousel_widgets[key] = widgets
        self._request_card_details(widgets)
        self._load_carousel_thumbs(key, result.items[:18])

    def _persist_recent_titles(self) -> None:
        self._settings.setValue(
            "home/recentTitles",
            json.dumps(
                [
                    {
                        "ref": recent.ref,
                        "title": recent.titles.display_name,
                        "poster": recent.poster_url if _is_remote_url(recent.poster_url) else "",
                    }
                    for recent in self._recent_items
                ],
                ensure_ascii=False,
            ),
        )

    def _load_carousel_thumbs(self, key: str, entries: list[AnimeSummary]) -> None:
        for anime in entries:
            if _poster_key(anime) in self._thumb_cache:
                continue
            # one task per card: the pool paints a whole feed at once instead of
            # waiting for the slowest poster in it
            self._runner.run_bulk(
                lambda item=anime: self._fetch_carousel_posters([item]),
                lambda fetched, item=anime: self._apply_carousel_entry(key, item, fetched),
                lambda message: log.debug("carousel poster unavailable: %s", message),
            )

    def _apply_carousel_entry(
        self, key: str, anime: AnimeSummary, fetched: dict[str, bytes]
    ) -> None:
        self._apply_carousel_thumbs(fetched)
        if _poster_key(anime) not in self._thumb_cache:
            self._resolve_carousel_posters(key, [anime])

    def _resolve_carousel_posters(self, key: str, entries: list[AnimeSummary]) -> None:
        """Recent cards persist a bare URL with no other artwork source, so a rotted or
        unusable one leaves the card permanently empty. Re-look it up by ref, once per
        session; a ref that fails then is left as-is rather than retried forever."""
        if key != "recent":
            return
        targets = [
            item.ref for item in entries if item.ref and item.ref not in self._poster_lookups
        ]
        if not targets:
            return
        self._poster_lookups.update(targets)
        for ref in targets:
            self._request_card_detail(ref)

    def _request_card_details(self, widgets: list[TitleCard]) -> None:
        """A list response carries only the planned episode count, so every card needs
        one detail request for its aired fraction. Each goes out as its own task, so the
        thread pool works through a whole feed at the client's advertised rate."""
        for card in widgets:
            self._request_card_detail(card.ref)

    def _request_card_detail(self, ref: str) -> None:
        if not ref:
            return
        if ref in self._detail_lookups:
            cached = self._card_details.get(ref)
            if cached is not None:
                self._apply_card_detail(ref, cached)
            return
        self._detail_lookups.add(ref)
        self._runner.run_bulk(
            lambda: self._repository.get_anime(ref),
            lambda detail, target=ref: self._apply_card_detail(target, detail),
            lambda message, target=ref: log.debug(
                "detail lookup failed for %s: %s", target, message
            ),
        )

    def _apply_card_detail(self, ref: str, detail: AnimeDetail) -> None:
        if not shiboken6.isValid(self):
            return
        self._card_details[ref] = detail
        self._queue_recent_poster_heal(ref, detail)
        for widgets in self._carousel_widgets.values():
            for card in widgets:
                if card.ref == ref:
                    card.apply_detail(detail)

    def _queue_recent_poster_heal(self, ref: str, detail: AnimeDetail) -> None:
        """A burst of detail results would otherwise re-render the recent feed once per
        title; one timer collapses them into a single repaint."""
        if not _is_remote_url(detail.cover_url):
            return
        if not any(item.ref == ref for item in self._carousel_entries.get("recent", [])):
            return
        self._healed_posters[ref] = detail.cover_url
        self._heal_timer.start()

    def _flush_recent_posters(self) -> None:
        healed, self._healed_posters = self._healed_posters, {}
        if not healed:
            return
        refreshed = [
            replace(item, poster=PosterSet(big=ImageVariant(webp=healed[item.ref])))
            if item.ref in healed
            else item
            for item in self._carousel_entries.get("recent", [])
        ]
        self._carousel_entries["recent"] = refreshed
        self._recent_items = refreshed[:12]
        self._persist_recent_titles()
        self._show_carousel("recent", SearchResult(items=refreshed))

    def _fetch_carousel_posters(self, entries: list[AnimeSummary]) -> dict[str, bytes]:
        fetched: dict[str, bytes] = {}
        for anime in entries:
            cache_key = anime.thumb_url or anime.poster_url
            if not cache_key:
                continue
            candidates = [
                anime.poster.small.webp,
                anime.poster.small.jpg,
                anime.poster.medium.webp,
                anime.poster.medium.jpg,
                anime.poster.big.webp,
                anime.poster.big.jpg,
            ]
            seen: set[str] = set()
            for url in candidates:
                if not url or url in seen:
                    continue
                seen.add(url)
                try:
                    fetched[cache_key] = self._repository.fetch_bytes(url)
                    break
                except Exception as exc:  # noqa: BLE001 - another poster variant may work.
                    log.debug("poster variant failed for %s (%s): %s", anime.ref, url, exc)
        return fetched

    def _apply_carousel_thumbs(self, fetched: dict[str, bytes]) -> None:
        self._thumb_cache.update(fetched)
        for url, data in fetched.items():
            pixmap = QPixmap()
            if not pixmap.loadFromData(data) or pixmap.isNull():
                continue
            # A poster URL can be shared by several cards and feeds.
            for cards in self._carousel_widgets.values():
                for card in cards:
                    if card.image_key == url:
                        card.apply_pixmap(pixmap)

    def refresh_feed_settings(self) -> None:
        """Apply a settings-dialog change: a feed switched off stops loading and hides,
        one switched on loads. Results still in flight for a hidden feed are dropped."""
        previous = self._enabled_feeds
        self._enabled_feeds = feeds.enabled(self._settings)
        if previous == self._enabled_feeds:
            return
        for key in feeds.ALL_FEEDS:
            self._feed_seq[key] += 1
            self._feed_section(key).setVisible(key in self._enabled_feeds)
        for key in set(feeds.ALL_FEEDS) - previous:
            self._load_feed(key)

    def _load_feed(self, key: str) -> None:
        if key in ("ongoing", "popular"):
            self._request_recommendation(key)
        elif key == "updates":
            self._load_updates()
        elif key == "schedule":
            self._load_schedule()

    def _load_ongoing(self) -> None:
        self._query.clear()
        self._browse_mode = True
        self._show_discovery()
        self._current_query = ""
        self._page_size = 24
        self._page = 1
        self._pages = 1
        self._entries = []
        self._results.clear()
        self._pager.hide()
        self._results_heading.setText("Рекомендації")
        self._status.setText(
            "Натисніть картку, щоб відкрити тайтл · пошук за назвою доступний вище"
        )
        for key in feeds.ALL_FEEDS:
            self._load_feed(key)

    def _request_recommendation(self, key: str) -> None:
        section = self._carousel_sections[key]
        self._feed_seq[key] += 1
        seq = self._feed_seq[key]
        if key not in self._enabled_feeds:
            return
        section._carousel_state.setText("Завантаження добірки…")
        section._carousel_retry.hide()
        if not self._carousel_entries.get(key):
            section._carousel_scroll.hide()
        browse = getattr(
            self._repository,
            "browse_ongoing" if key == "ongoing" else "browse_popular",
            None,
        )
        if browse is None and key == "popular":
            section._carousel_state.setText("Ця добірка недоступна для цього джерела даних.")
            return
        request = lambda: (
            browse(page=1, limit=18) if browse else self._repository.search("", page=1, limit=18)
        )

        def on_result(result: SearchResult) -> None:
            if seq != self._feed_seq[key]:
                return
            self._rate_label.refresh()
            self._show_carousel(key, result)

        def on_error(message: str) -> None:
            if seq != self._feed_seq[key]:
                return
            self._rate_label.refresh()
            section._carousel_state.setText(f"Не вдалося завантажити: {message}")
            section._carousel_retry.show()

        self._runner.run(request, on_result, on_error)

    def _load_updates(self) -> None:
        if "updates" not in self._enabled_feeds:
            return
        section = self._feed_sections["updates"]
        self._feed_seq["updates"] += 1
        seq = self._feed_seq["updates"]
        getter = getattr(self._repository, "get_updates", None)
        if getter is None:
            section._feed_state.setText("Стрічка оновлень недоступна для цього джерела даних.")
            section._feed_rows_host.hide()
            return
        section._feed_state.setText("Завантаження нових серій…")
        section._feed_retry.setEnabled(False)

        def on_result(result: UpdatesResult) -> None:
            if seq != self._feed_seq["updates"]:
                return
            self._rate_label.refresh()
            section._feed_retry.setEnabled(True)
            rows = []
            for update in result.items[:5]:
                anime = update.anime
                if not anime.ref:
                    continue
                team = update.team_name or ", ".join(
                    entry.name for entry in update.teams if entry.name
                )
                kind = {"voice": "озвучення", "sub": "субтитри"}.get(update.kind, update.kind)
                parts = [f"Серія {update.episode}" if update.episode is not None else "Нова серія"]
                if team:
                    parts.append(team)
                if kind:
                    parts.append(kind)
                if update.provider:
                    parts.append(update.provider.capitalize())
                when = self._format_feed_time(update.published_at)
                if when:
                    parts.append(when)
                rows.append((anime, " · ".join(parts)))
            self._show_feed_rows(
                "updates",
                rows,
                f"{len(rows)} останніх оновлень" if rows else "Нових оновлень поки немає.",
            )

        def on_error(message: str) -> None:
            if seq != self._feed_seq["updates"]:
                return
            self._rate_label.refresh()
            section._feed_retry.setEnabled(True)
            section._feed_state.setText(f"Не вдалося завантажити оновлення: {message}")
            section._feed_rows_host.hide()

        self._runner.run(lambda: getter(page=1, limit=12), on_result, on_error)

    def _load_schedule(self) -> None:
        if "schedule" not in self._enabled_feeds:
            return
        section = self._feed_sections["schedule"]
        self._feed_seq["schedule"] += 1
        seq = self._feed_seq["schedule"]
        getter = getattr(self._repository, "get_schedule", None)
        if getter is None:
            section._feed_state.setText("Розклад недоступний для цього джерела даних.")
            section._feed_rows_host.hide()
            return
        section._feed_state.setText("Завантаження розкладу…")
        section._feed_retry.setEnabled(False)

        def on_result(result: ScheduleResult) -> None:
            if seq != self._feed_seq["schedule"]:
                return
            self._rate_label.refresh()
            section._feed_retry.setEnabled(True)
            days = (
                ("monday", "Пн"),
                ("tuesday", "Вт"),
                ("wednesday", "Ср"),
                ("thursday", "Чт"),
                ("friday", "Пт"),
                ("saturday", "Сб"),
                ("sunday", "Нд"),
            )
            today = datetime.now().weekday()
            ordered_days = days[today:] + days[:today]
            rows = []
            for key, label in ordered_days:
                entries = sorted(result.days.get(key, []), key=lambda entry: entry.airing)
                for entry in entries:
                    if entry.finished or not entry.anime.ref:
                        continue
                    when = self._format_feed_time(entry.airing, time_only=True)
                    details = [label]
                    if when:
                        details.append(when)
                    if entry.episode is not None:
                        details.append(f"Серія {entry.episode}")
                    rows.append((entry.anime, " · ".join(details)))
                    if len(rows) >= 5:
                        break
                if len(rows) >= 5:
                    break
            self._show_feed_rows(
                "schedule",
                rows,
                f"{len(rows)} найближчих виходів" if rows else "У розкладі поки немає виходів.",
            )

        def on_error(message: str) -> None:
            if seq != self._feed_seq["schedule"]:
                return
            self._rate_label.refresh()
            section._feed_retry.setEnabled(True)
            section._feed_state.setText(f"Не вдалося завантажити розклад: {message}")
            section._feed_rows_host.hide()

        self._runner.run(getter, on_result, on_error)

    def _show_feed_rows(
        self, key: str, entries: list[tuple[AnimeSummary, str]], empty_text: str
    ) -> None:
        section = self._feed_sections[key]
        rows = self._feed_rows[key]
        while rows.count():
            item = rows.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
        if not entries:
            self._feed_poster_labels[key] = []
            section._feed_state.setText(empty_text)
            section._feed_state.show()
            section._feed_rows_host.hide()
            return
        section._feed_state.setText(empty_text)
        section._feed_state.hide()
        posters: list[tuple[str, QLabel]] = []
        for anime, details in entries:
            row = QWidget()
            row.setObjectName("homeFeedRow")
            row_layout = QHBoxLayout(row)
            row_layout.setContentsMargins(10, 4, 10, 4)
            row_layout.setSpacing(12)
            thumb = anime.thumb_url
            if thumb:
                poster = QLabel(row)
                poster.setObjectName("homeFeedPoster")
                poster.setFixedSize(HOME_FEED_POSTER_WIDTH, HOME_FEED_POSTER_HEIGHT)
                poster.setAlignment(Qt.AlignmentFlag.AlignCenter)
                poster.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
                posters.append((thumb, poster))
                self._apply_poster(thumb, poster)
                row_layout.addWidget(poster, 0, Qt.AlignmentFlag.AlignVCenter)
            title = QPushButton(anime.titles.display_name)
            title.setObjectName("homeFeedTitle")
            title.setToolTip(anime.titles.display_name)
            title.clicked.connect(
                lambda _checked=False, ref=anime.ref, poster=anime.poster_url: (
                    self.open_requested.emit(ref, poster)
                )
            )
            row.setContextMenuPolicy(Qt.CustomContextMenu)
            row.customContextMenuRequested.connect(
                lambda pos, ref=anime.ref, host=row: self._anime_card_menu(
                    ref, host.mapToGlobal(pos)
                )
            )
            meta = QLabel(details)
            meta.setObjectName("homeFeedMeta")
            meta.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            row_layout.addWidget(title, 1)
            row_layout.addWidget(meta)
            rows.addWidget(row)
        section._feed_rows_host.show()
        self._feed_poster_labels[key] = posters
        self._request_feed_posters()

    @staticmethod
    def _format_feed_time(timestamp: str, *, time_only: bool = False) -> str:
        if not timestamp:
            return ""
        try:
            value = datetime.fromisoformat(timestamp.replace("Z", "+00:00")).astimezone()
        except ValueError:
            return ""
        return value.strftime("%H:%M" if time_only else "%d.%m · %H:%M")

    def _fetch_page(self, page: int) -> None:
        self._status.setText("Завантаження добірки…" if self._browse_mode else "Пошук…")
        self._previous.setEnabled(False)
        self._next.setEnabled(False)
        if not self._browse_mode:
            self._run_results_request(
                lambda: self._repository.search(
                    self._current_query, page=page, limit=self._page_size
                ),
                page,
            )
            return
        browse = getattr(self._repository, "browse_ongoing", None)
        if browse is None:
            self._run_results_request(
                lambda: self._repository.search("", page=page, limit=self._page_size), page
            )
        else:
            self._run_results_request(lambda: browse(page=page, limit=self._page_size), page)

    def _run_results_request(self, fn, requested_page: int) -> None:
        self._request_seq += 1
        request_seq = self._request_seq

        def on_results(result: SearchResult) -> None:
            if request_seq == self._request_seq:
                self._on_results(result, requested_page)

        def on_error(message: str) -> None:
            if request_seq == self._request_seq:
                self._on_error(message)

        self._runner.run(fn, on_results, on_error)

    def _search(self) -> None:
        query = self._query.text().strip()
        if not query:
            self._load_ongoing()
            return
        self._browse_mode = False
        self._current_query = query
        self._page_size = 20
        self._page = 1
        self._pages = 1
        self._entries = []
        self._results.clear()
        self._pager.hide()
        self._results_heading.setText("Результати пошуку")
        self._show_search_results()
        self._fetch_page(1)

    def _show_discovery(self) -> None:
        self._discovery.show()
        self._results.hide()
        self._format_bar.hide()
        self._results_heading.show()

    def _show_search_results(self) -> None:
        self._discovery.hide()
        self._results.show()
        self._results_heading.show()

    def _on_results(self, result: SearchResult, requested_page: int = 1) -> None:
        self._rate_label.refresh()
        self._page = result.page or requested_page
        self._pages = max(
            1,
            result.pages
            if result.pages is not None
            else ((result.total or len(result.items)) + self._page_size - 1) // self._page_size,
        )
        self._entries = result.items
        total = result.total
        if not self._entries:
            self._status.setText("Нічого не знайдено")
        else:
            self._status.setText(f"Знайдено: {total if total is not None else len(self._entries)}")
        self._rebuild_format_bar()
        self._populate_rows()
        self._update_pager()
        needed = sorted(
            thumb
            for thumb in {entry.thumb_url for entry in self._entries}
            if thumb and thumb not in self._thumb_cache
        )
        if needed:
            self._runner.run(
                lambda: self._fetch_thumbs(needed),
                self._apply_thumbs,
                self._thumbs_failed,
            )

    def _rebuild_format_bar(self) -> None:
        counts: dict[str, int] = {}
        for entry in self._entries:
            fmt = entry.format or "unknown"
            counts[fmt] = counts.get(fmt, 0) + 1
        order = ("tv", "movie", "special", "ova", "ona", "music", "other", "unknown")
        present = [fmt for fmt in order if counts.get(fmt)]
        self._format_bar.blockSignals(True)
        while self._format_bar.count():
            self._format_bar.removeTab(0)
        if len(present) >= 2:
            self._format_bar.addTab(f"Усі ({len(self._entries)})")
            self._format_bar.setTabData(0, "all")
            for index, fmt in enumerate(present, start=1):
                self._format_bar.addTab(f"{fmt.upper()} ({counts[fmt]})")
                self._format_bar.setTabData(index, fmt)
            self._format_bar.setCurrentIndex(0)
            self._format_bar.show()
        else:
            self._format_bar.hide()
        self._format_bar.blockSignals(False)

    def _apply_format_filter(self) -> None:
        self._populate_rows()

    def _populate_rows(self) -> None:
        self._results.clear()
        wanted = "all"
        if self._format_bar.isVisible():
            index = self._format_bar.currentIndex()
            data = self._format_bar.tabData(index) if index >= 0 else None
            if isinstance(data, str):
                wanted = data
        for entry in self._entries:
            if wanted != "all" and (entry.format or "unknown") != wanted:
                continue
            year = entry.year
            meta = " · ".join(
                part for part in (str(year) if year else "", (entry.format or "").upper()) if part
            )
            name = entry.titles.display_name
            label = f"{name}  ·  {meta}" if meta else name
            thumb = entry.thumb_url
            item = QListWidgetItem(label)
            item.setToolTip(label)
            item.setData(
                Qt.ItemDataRole.UserRole,
                {"ref": entry.ref, "poster": entry.poster_url, "thumb": thumb},
            )
            icon = self._icon_for(thumb)
            if icon is not None:
                item.setIcon(icon)
            self._results.addItem(item)

    def _icon_for(self, thumb: str | None) -> QPixmap | None:
        if not thumb or thumb not in self._thumb_cache:
            return None
        if thumb not in self._icons:
            pixmap = QPixmap()
            if not pixmap.loadFromData(self._thumb_cache[thumb]) or pixmap.isNull():
                return None
            self._icons[thumb] = pixmap
        return self._icons[thumb]

    def _fetch_thumbs(self, urls: list[str]) -> dict[str, bytes]:
        fetched: dict[str, bytes] = {}
        for url in urls:
            try:
                fetched[url] = self._repository.fetch_bytes(url)
            except Exception as exc:  # noqa: BLE001 - thumbnails are optional to search results.
                log.warning("thumbnail fetch failed for %s: %s", url, exc)
        return fetched

    def _apply_thumbs(self, fetched: dict[str, bytes]) -> None:
        self._thumb_cache.update(fetched)
        for row in range(self._results.count()):
            item = self._results.item(row)
            data = item.data(Qt.ItemDataRole.UserRole) or {}
            icon = self._icon_for(data.get("thumb"))
            if icon is not None:
                item.setIcon(icon)

    def _apply_poster(self, thumb: str, label: QLabel) -> None:
        icon = self._icon_for(thumb)
        if icon is None:
            return
        label.setPixmap(
            icon.scaled(
                label.size(),
                Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                Qt.TransformationMode.SmoothTransformation,
            )
        )

    def _request_feed_posters(self) -> None:
        needed = sorted(
            {
                thumb
                for posters in self._feed_poster_labels.values()
                for thumb, _ in posters
                if thumb and thumb not in self._thumb_cache
            }
        )
        if needed:
            self._runner.run(
                lambda: self._fetch_thumbs(needed),
                self._apply_feed_posters,
                self._thumbs_failed,
            )

    def _apply_feed_posters(self, fetched: dict[str, bytes]) -> None:
        self._thumb_cache.update(fetched)
        for posters in self._feed_poster_labels.values():
            for thumb, label in posters:
                if shiboken6.isValid(label):
                    self._apply_poster(thumb, label)

    def _thumbs_failed(self, message: str) -> None:
        log.warning("thumbnail batch failed: %s", message)

    def _on_error(self, message: str) -> None:
        self._rate_label.refresh()
        self._status.setText(message)
        self._update_pager()

    def _update_pager(self) -> None:
        self._pager.setVisible(self._pages > 1)
        self._page_label.setText(f"{self._page} / {self._pages}")
        self._previous.setEnabled(self._page > 1)
        self._next.setEnabled(self._page < self._pages)

    def _change_page(self, delta: int) -> None:
        page = self._page + delta
        if 1 <= page <= self._pages:
            self._fetch_page(page)

    def _open(self, item: QListWidgetItem) -> None:
        data = item.data(Qt.ItemDataRole.UserRole) or {}
        ref = data.get("ref")
        if ref:
            self.open_requested.emit(ref, data.get("poster"))

    def _results_context_menu(self, pos) -> None:
        item = self._results.itemAt(pos)
        if item is None:
            return
        data = item.data(Qt.ItemDataRole.UserRole) or {}
        ref = data.get("ref")
        if not ref:
            return
        menu = QMenu(self)
        open_action = menu.addAction("Відкрити")
        open_action.setToolTip("Відкрити сторінку тайтла")
        open_action.triggered.connect(lambda _=False, current=item: self._open(current))
        copy_link = menu.addAction("Копіювати посилання")
        copy_link.setToolTip("Скопіювати адресу сторінки на mikai.me")
        copy_link.triggered.connect(lambda _=False, target=ref: self._copy_anime_link(target))
        copy_ref = menu.addAction("Копіювати ID")
        copy_ref.setToolTip("Скопіювати ідентифікатор тайтла")
        copy_ref.triggered.connect(lambda _=False, target=ref: self._copy_ref(target))
        menu.exec(self._results.mapToGlobal(pos))

    def _anime_card_menu(self, ref: str, global_pos) -> None:
        menu = QMenu(self)
        open_action = menu.addAction("Відкрити")
        open_action.setToolTip("Відкрити сторінку тайтла")
        open_action.triggered.connect(lambda _=False, target=ref: self._open_by_ref(target))
        copy_link = menu.addAction("Копіювати посилання")
        copy_link.setToolTip("Скопіювати адресу сторінки на mikai.me")
        copy_link.triggered.connect(lambda _=False, target=ref: self._copy_anime_link(target))
        copy_ref = menu.addAction("Копіювати ID")
        copy_ref.setToolTip("Скопіювати ідентифікатор тайтла")
        copy_ref.triggered.connect(lambda _=False, target=ref: self._copy_ref(target))
        browser_action = menu.addAction("Відкрити в браузері")
        browser_action.setToolTip("Відкрити сторінку на mikai.me")
        browser_action.triggered.connect(
            lambda _=False, target=ref: QDesktopServices.openUrl(QUrl(anime_page_url(target)))
        )
        menu.exec(global_pos)

    def _open_by_ref(self, ref: str) -> None:
        entries = list(self._carousel_entries.get("ongoing", []))
        entries.extend(self._carousel_entries.get("popular", []))
        entries.extend(self._recent_items)
        entries.extend(self._entries)
        for entry in entries:
            if entry.ref == ref:
                self.open_requested.emit(ref, entry.poster_url)
                return
        self.open_requested.emit(ref, None)

    @staticmethod
    def _copy_anime_link(ref: str) -> None:
        QGuiApplication.clipboard().setText(anime_page_url(ref))

    @staticmethod
    def _copy_ref(ref: str) -> None:
        QGuiApplication.clipboard().setText(ref)


class MainWindow(QWidget):
    def __init__(self, repository: AnimeRepository) -> None:
        super().__init__()
        self._repository = repository
        self._runner = Runner(self)
        self._title: TitlePage | None = None
        self._title_ref: str | None = None
        self._title_poster: str | None = None
        self._title_history: list[tuple[str, str | None]] = []

        self.setWindowTitle("Mikai Wrapper")
        self.setMinimumSize(860, 620)
        self.resize(1080, 760)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        self._stack = QStackedWidget()
        root.addWidget(self._stack)

        self._search = SearchPage(self._runner, repository)
        self._search.open_requested.connect(self.open_title)
        self._search.settings_requested.connect(self.open_settings)
        self._stack.addWidget(self._search)

    def open_settings(self) -> None:
        dlg = SettingsDialog(self._repository, self._runner, self)
        dlg.exec()
        if dlg.result() == QDialog.DialogCode.Accepted:
            self._search.refresh_feed_settings()

    def open_title(self, ref: str, poster: object, *, preserve_history: bool = False) -> None:
        if not preserve_history:
            self._title_history.clear()
        self._dispose_title()
        self._title_ref = ref
        self._title_poster = poster if isinstance(poster, str) else None
        self._title = TitlePage(self._runner, self._repository, ref, self._title_poster)
        self._title.recent_title_loaded.connect(self._search.record_recent_title)
        self._title.back_requested.connect(self.go_search)
        self._title.settings_requested.connect(self.open_settings)
        self._title.related_open_requested.connect(self._open_related)
        self._stack.addWidget(self._title)
        self._stack.setCurrentWidget(self._title)

    def _open_related(self, ref: str, poster: object) -> None:
        if self._title_ref is not None:
            self._title_history.append((self._title_ref, self._title_poster))
        self.open_title(ref, poster, preserve_history=True)

    def go_search(self) -> None:
        if self._title_history:
            ref, poster = self._title_history.pop()
            self.open_title(ref, poster, preserve_history=True)
            return
        self._dispose_title()
        self._title_ref = None
        self._title_poster = None
        self._stack.setCurrentWidget(self._search)

    def _dispose_title(self) -> None:
        if self._title is None:
            return
        self._title.shutdown()
        self._stack.removeWidget(self._title)
        self._title.deleteLater()
        self._title = None

    def closeEvent(self, event) -> None:
        self._dispose_title()
        super().closeEvent(event)


def main() -> None:
    app = QApplication(sys.argv)
    app.setApplicationName("Mikai Wrapper")
    app.setStyle("Fusion")
    app.setStyleSheet(build_stylesheet())
    window = MainWindow(MikaiClient())
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
