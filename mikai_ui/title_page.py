from __future__ import annotations

import logging
import time
from pathlib import Path

import shiboken6
from PySide6.QtCore import QProcess, QSettings, QSize, QStandardPaths, Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QDesktopServices, QPixmap
from PySide6.QtWidgets import (
    QComboBox,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMenu,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QStyle,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from mikai_backend import AnimeDetail, AnimeRepository, AnimeSummary, PlayerResult, Release, streams

from mikai_ui import quality_cache
from mikai_ui.clipboard import copy_text
from mikai_ui.cover_viewer import CoverLabel, CoverViewer
from mikai_backend.releases import playable_episode_count, sort_releases
from mikai_ui.labels import STATUS_LABELS
from mikai_ui.mpv_backend import prepare_runtime
from mikai_ui.page_pager import RangePager
from mikai_ui.player_dialog import PlayerDialog
from mikai_ui.rate_limit_label import RateLimitLabel
from mikai_ui.settings_dialog import make_gear_icon
from mikai_ui.theme import ACCENT, BORDER, MUTED, TEXT
from mikai_ui.theme.metrics import (
    CAROUSEL_CARD_HEIGHT,
    CAROUSEL_CARD_WIDTH,
    CAROUSEL_POSTER_HEIGHT,
    CAROUSEL_POSTER_WIDTH,
    CAROUSEL_SCROLL_STEP,
    EPISODE_BUTTON_HEIGHT,
    EPISODE_BUTTON_WIDTH,
    EPISODE_SECTION_MAX_HEIGHT,
    EPISODES_PER_PAGE,
    QUALITY_CACHE_SAVE_DELAY_MS,
    SPACING_SMALL,
    TITLE_COVER_HEIGHT,
    TITLE_COVER_WIDTH,
    TITLE_DESCRIPTION_PREVIEW_CHARS,
)
from mikai_ui.workers import Runner

log = logging.getLogger(__name__)

KIND_LABELS = {"voice": "Озвучення", "sub": "Субтитри"}
PROVIDER_LABELS = {"ashdi": "Ashdi", "moon": "MoonAnime", "tortuga": "Tortuga"}
FORMAT_LABELS = {
    "tv": "ТБ серіал",
    "movie": "Фільм",
    "ova": "OVA",
    "ona": "Web-серіал",
    "special": "Спецвипуск",
    "music": "Музичне відео",
    "other": "Інше",
}
RELATION_LABELS = {
    "prequel": "Передісторія",
    "sequel": "Продовження",
    "sideStory": "Побічна історія",
    "parentStory": "Основна історія",
    "spinOff": "Спіноф",
    "alternativeVersion": "Інша версія",
    "alternativeSetting": "Інший сетинг",
    "summary": "Переказ",
    "fullStory": "Повна історія",
    "adaptation": "Адаптація",
    "other": "Інше",
    "similar": "Схоже",
}
SOURCE_LABELS = {
    "manga": "Манґа",
    "novel": "Роман",
    "light_novel": "Легка новела",
    "web_comic": "Вебкомікс",
    "visual_novel": "Візуальна новела",
    "other": "Інше",
}
COUNTRY_LABELS = {
    "JP": "Японія",
    "KR": "Південна Корея",
    "CN": "Китай",
}
AGE_LABELS = {
    "all": "Для всіх",
    "any": "Для всіх",
    "g": "Для всіх",
    "pg": "Для всіх",
    "pg12": "12+",
    "pg13": "13+",
    "pg16": "16+",
    "r17": "17+",
    "r18": "18+",
}
SEASON_LABELS = {
    12: "Зима",
    1: "Зима",
    2: "Зима",
    3: "Весна",
    4: "Весна",
    5: "Весна",
    6: "Літо",
    7: "Літо",
    8: "Літо",
    9: "Осінь",
    10: "Осінь",
    11: "Осінь",
}


def _season_label(start_date: str) -> str | None:
    try:
        year, month, _ = (int(part) for part in start_date.split("-"))
    except AttributeError, ValueError:
        return None
    season = SEASON_LABELS.get(month)
    return f"{season} {year}" if season else str(year)


def _compact_number(value: int) -> str:
    if value >= 1_000_000:
        return f"{value / 1_000_000:.1f}M".replace(".0M", "M")
    if value >= 1_000:
        return f"{value / 1_000:.1f}K".replace(".0K", "K")
    return str(value)


class EpisodeGrid(QWidget):
    """Reflow the current page of episodes as the window width changes."""

    layout_changed = Signal()

    def __init__(self) -> None:
        super().__init__()
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self._grid = QGridLayout(self)
        self._grid.setContentsMargins(0, SPACING_SMALL, 0, 0)
        self._grid.setSpacing(SPACING_SMALL)
        self._cards: list[QWidget] = []
        self._columns = 0
        self._rows = 0

    def set_cards(self, cards: list[QWidget]) -> None:
        while self._grid.count():
            item = self._grid.takeAt(0)
            if item.widget() is not None:
                item.widget().deleteLater()
        self._cards = cards
        self._columns = 0
        self._rows = 0
        self._reflow()

    def set_layout_width(self, width: int) -> None:
        self.resize(width, self.height())
        self._reflow()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._reflow()

    def _reflow(self) -> None:
        # one compact pill per episode: 72px plus the gutter fits a row of 14
        columns = max(1, self.width() // (EPISODE_BUTTON_WIDTH + SPACING_SMALL))
        if columns == self._columns:
            return
        previous = self._columns
        while self._grid.count():
            self._grid.takeAt(0)
        for index, card in enumerate(self._cards):
            self._grid.addWidget(
                card,
                index // columns,
                index % columns,
                alignment=Qt.AlignmentFlag.AlignTop,
            )
        rows = (len(self._cards) + columns - 1) // columns
        for column in range(max(previous, columns)):
            self._grid.setColumnStretch(column, 1 if column < columns else 0)
        for row in range(max(self._rows, rows)):
            self._grid.setRowStretch(row, 0)
        self._columns = columns
        self._rows = rows
        self.updateGeometry()
        self.layout_changed.emit()


class TitlePage(QWidget):
    back_requested = Signal()
    settings_requested = Signal()
    related_open_requested = Signal(str, object)
    recent_title_loaded = Signal(str, str, str)

    def __init__(
        self,
        runner: Runner,
        repository: AnimeRepository,
        ref: str,
        poster_hint: str | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._runner = runner
        self._repository = repository
        self._ref = ref
        self._poster_hint = poster_hint
        self._closed = False
        self._releases: list[Release] = []
        self._base_status = ""
        self._tab_embeds: list[list[str]] = []
        self._tab_states: list[dict] = []
        self._player_dialogs: list[PlayerDialog] = []
        self._cover_pixmap = QPixmap()
        self._cover_viewer: CoverViewer | None = None
        self._cover_url = ""
        self._settings = QSettings("Mikai", "Mikai Wrapper")
        self._quality_cache, self._quality_fetched_at = quality_cache.load(self._settings)
        self._cache_save_timer = QTimer(self)
        self._cache_save_timer.setSingleShot(True)
        self._cache_save_timer.setInterval(QUALITY_CACHE_SAVE_DELAY_MS)
        self._cache_save_timer.timeout.connect(self._save_quality_cache)
        self._no_source_urls: set[str] = set()
        self._fetch_waiters: dict[str, list] = {}
        self._full_description = ""
        self._description_expanded = False
        self._build_ui()
        self._load()

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        top = QHBoxLayout()
        back = QPushButton("← Назад")
        back.clicked.connect(self.back_requested)
        self._status = QLabel("")
        self._status.setObjectName("status")

        self._settings_btn = QPushButton()
        self._settings_btn.setObjectName("settingsChip")
        self._settings_btn.setIcon(make_gear_icon(18, TEXT))
        self._settings_btn.setIconSize(QSize(18, 18))
        self._settings_btn.setToolTip("Налаштування токена API")
        self._settings_btn.clicked.connect(self.settings_requested)

        self._rate_label = RateLimitLabel(self._repository)

        top.addWidget(back)
        top.addStretch()
        top.addWidget(self._status)
        top.addWidget(self._rate_label)
        top.addWidget(self._settings_btn)
        root.addLayout(top)

        scroll = QScrollArea()
        scroll.setObjectName("titleOverview")
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._overview_scroll = scroll
        page = QWidget()
        page_layout = QHBoxLayout(page)
        page_layout.setContentsMargins(0, 0, 0, 0)
        page_layout.setSpacing(24)

        self._cover = CoverLabel("Завантаження…")
        self._cover.setObjectName("titleCover")
        self._cover.setFixedSize(TITLE_COVER_WIDTH, TITLE_COVER_HEIGHT)
        self._cover.activated.connect(self._open_cover_viewer)
        self._cover.setContextMenuPolicy(Qt.CustomContextMenu)
        self._cover.customContextMenuRequested.connect(
            lambda pos: self._cover_context_menu(self._cover.mapToGlobal(pos))
        )
        page_layout.addWidget(self._cover, alignment=Qt.AlignmentFlag.AlignTop)

        text_col = QVBoxLayout()
        text_col.setSpacing(8)
        self._heading = QLabel("")
        self._heading.setObjectName("heading")
        self._heading.setWordWrap(True)
        self._subtitle = QLabel("")
        self._subtitle.setObjectName("muted")
        self._subtitle.setWordWrap(True)
        text_col.addWidget(self._heading)
        text_col.addWidget(self._subtitle)

        self._facts = QLabel("")
        self._facts.setObjectName("titleFacts")
        self._facts.setWordWrap(True)
        text_col.addWidget(self._facts)

        self._scores = QLabel("")
        self._scores.setStyleSheet(f"color: {ACCENT}; font-weight: 600;")
        text_col.addWidget(self._scores)

        self._description = QLabel("")
        self._description.setObjectName("titleDescription")
        self._description.setTextFormat(Qt.TextFormat.PlainText)
        self._description.setWordWrap(True)
        self._description.setAlignment(Qt.AlignmentFlag.AlignTop)
        self._description.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        text_col.addWidget(self._description)

        disclosure_row = QHBoxLayout()
        self._description_button = QPushButton("Читати далі")
        self._description_button.setObjectName("textAction")
        self._description_button.clicked.connect(self._toggle_description)
        self._description_button.hide()
        disclosure_row.addWidget(self._description_button)

        self._meta_button = QPushButton("Докладніше про тайтл")
        self._meta_button.setObjectName("textAction")
        self._meta_button.setCheckable(True)
        self._meta_button.toggled.connect(self._toggle_metadata)
        disclosure_row.addWidget(self._meta_button)

        self._related_button = QPushButton("Пов’язані та схожі")
        self._related_button.setObjectName("textAction")
        self._related_button.setCheckable(True)
        self._related_button.toggled.connect(self._related_host_visible)
        self._related_button.hide()
        disclosure_row.addWidget(self._related_button)
        disclosure_row.addStretch()
        text_col.addLayout(disclosure_row)

        meta_host = QWidget()
        meta_host.setObjectName("titleDetails")
        self._meta_host = meta_host
        self._meta_grid = QGridLayout(meta_host)
        self._meta_grid.setContentsMargins(12, 12, 12, 12)
        self._meta_grid.setHorizontalSpacing(12)
        self._meta_grid.setVerticalSpacing(6)
        self._meta_grid.setColumnStretch(1, 1)
        meta_host.hide()
        text_col.addWidget(meta_host)

        self._links = QLabel("")
        self._links.setOpenExternalLinks(True)
        self._links.setStyleSheet(f"color: {ACCENT};")
        text_col.addWidget(self._links)

        self._related_host = QWidget()
        self._related_layout = QVBoxLayout(self._related_host)
        self._related_layout.setContentsMargins(0, 0, 0, 0)
        self._related_layout.setSpacing(4)
        self._related_host.hide()
        text_col.addWidget(self._related_host)
        text_col.addStretch()
        page_layout.addLayout(text_col, stretch=1)

        scroll.setWidget(page)
        root.addWidget(scroll, stretch=1)

        selector_row = QHBoxLayout()
        selector_row.setSpacing(8)
        episodes_heading = QLabel("Серії")
        episodes_heading.setObjectName("sectionHeading")
        selector_row.addWidget(episodes_heading)
        self._selector = QComboBox()
        self._selector.setEnabled(False)
        self._selector.currentIndexChanged.connect(self._fill_episodes)
        selector_row.addWidget(self._selector, stretch=1)
        self._copy_all = QPushButton("Копіювати всі")
        self._copy_all.setEnabled(False)
        self._copy_all.clicked.connect(self._copy_all_urls)
        selector_row.addWidget(self._copy_all)
        root.addLayout(selector_row)

        self._tabs = QTabWidget()
        self._tabs.currentChanged.connect(self._tab_changed)
        self._tabs.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        root.addWidget(self._tabs)
        self._fit_episode_section()

        self._url = QLineEdit()
        self._url.setReadOnly(True)
        self._url.hide()
        root.addWidget(self._url)

    def _load(self) -> None:
        self._set_status("Завантаження…")

        def fetch() -> tuple[AnimeDetail, PlayerResult]:
            return self._repository.get_anime(self._ref), self._repository.get_player(self._ref)

        self._runner.run(fetch, self._on_loaded, self._on_error)

    def _on_loaded(self, payload: tuple[AnimeDetail, PlayerResult]) -> None:
        if self._closed or not shiboken6.isValid(self):
            return
        self._rate_label.refresh()
        detail, player = payload
        heading = detail.titles.display_name or self._ref
        subtitle = detail.titles.english or detail.titles.original or ""
        self._heading.setText(heading)
        self._subtitle.setText(subtitle)
        self._populate_detail(detail)
        self.recent_title_loaded.emit(
            detail.ref or self._ref,
            heading,
            detail.cover_url or self._poster_hint or "",
        )
        self._load_recommendations(detail)
        self._full_description = (detail.description or "Опис відсутній.").strip()
        self._description_expanded = False
        self._description.setText(self._description_preview())
        self._description_button.setText("Читати далі")
        self._description_button.setVisible(
            len(self._full_description) > TITLE_DESCRIPTION_PREVIEW_CHARS
        )

        cover_url = detail.cover_url
        if cover_url:
            self._load_cover(cover_url, allow_hint=True)
        elif self._poster_hint:
            self._load_cover(self._poster_hint, allow_hint=False)
        else:
            self._cover.setText("Без обкладинки")

        self._releases = sort_releases(player.releases)
        self._selector.blockSignals(True)
        self._selector.clear()
        for index, release in enumerate(self._releases):
            self._selector.addItem(self._release_label(release), index)
        if self._releases:
            self._selector.setCurrentIndex(0)
        self._selector.blockSignals(False)

        if self._releases:
            self._selector.setEnabled(True)
            self._fill_episodes(0)
        else:
            self._set_status("Релізів немає")

    def _populate_detail(self, detail: AnimeDetail) -> None:
        self._scores.setText(self._scores_text(detail))
        self._populate_related(detail)

        while self._meta_grid.count():
            item = self._meta_grid.takeAt(0)
            widget = item.widget()
            if widget is not None:
                widget.deleteLater()

        episodes = detail.episodes
        if episodes.aired or episodes.localized:
            total = episodes.total or 0
            localized = episodes.localized or episodes.aired
            if total and total != localized:
                episodes_label = f"{localized} / {total}"
            else:
                episodes_label = str(localized)
        else:
            episodes_label = None

        facts = [
            STATUS_LABELS.get(detail.status, detail.status or ""),
            FORMAT_LABELS.get(detail.format, detail.format or ""),
            _season_label(detail.start_date) or "",
            f"{episodes_label} серій" if episodes_label else "",
        ]
        self._facts.setText("   ·   ".join(part for part in facts if part))

        rows = [
            ("Серій", episodes_label),
            ("Тип", FORMAT_LABELS.get(detail.format, detail.format or None)),
            (
                "Тривалість",
                f"{episodes.duration_min} хв" if episodes.duration_min else None,
            ),
            ("Статус", STATUS_LABELS.get(detail.status, detail.status or None)),
            ("Сезон", _season_label(detail.start_date)),
            ("Першоджерело", SOURCE_LABELS.get(detail.source, detail.source or None)),
            ("Студія", detail.studio),
            ("Країна", COUNTRY_LABELS.get(detail.country, detail.country or None)),
            ("Вікові обмеження", AGE_LABELS.get(detail.age_rating, detail.age_rating or None)),
            (
                "Жанри",
                ", ".join(item.display for item in detail.genres if item.display) or None,
            ),
            (
                "Теги",
                ", ".join(item.display for item in detail.tags if item.display) or None,
            ),
        ]
        for row, (label, value) in enumerate(row for row in rows if row[1]):
            key = QLabel(label)
            key.setObjectName("muted")
            key.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignTop)
            val = QLabel(value)
            val.setWordWrap(True)
            self._meta_grid.addWidget(key, row, 0)
            self._meta_grid.addWidget(val, row, 1)

        links = list(detail.external_links)
        slug = detail.ids.get("slug") or detail.ref
        if slug:
            links.insert(0, ("Mikai", f"https://mikai.me/anime/{slug}"))
        self._links.setText(
            " · ".join(
                f'<a style="text-decoration: none;" href="{url}">{name}</a>' for name, url in links
            )
        )

    def _populate_related(self, detail: AnimeDetail) -> None:
        while self._related_layout.count():
            item = self._related_layout.takeAt(0)
            widget = item.widget()
            if widget is not None:
                widget.deleteLater()

        relations = [
            (relation.kind, relation.anime)
            for relation in detail.relations[:6]
            if relation.anime.ref
        ]
        similar = [("similar", anime) for anime in detail.similar[:6] if anime.ref]
        sections: list[tuple[str, list[tuple[str, AnimeSummary]]]] = []
        if relations:
            sections.append(("Пов’язані тайтли", relations))
        if similar:
            sections.append(("Схожі аніме", similar))
        if not sections:
            self._related_host.hide()
            self._related_button.hide()
            return

        for heading, entries in sections:
            self._add_related_carousel(heading, entries)
        self._set_related_expanded(bool(sections))

    def _load_recommendations(self, detail: AnimeDetail) -> None:
        browse = getattr(self._repository, "browse_popular", None)
        if browse is None:
            return

        def on_ok(result) -> None:
            if self._closed or not shiboken6.isValid(self):
                return
            excluded = {self._ref, detail.ref}
            excluded.update(
                relation.anime.ref for relation in detail.relations if relation.anime.ref
            )
            excluded.update(anime.ref for anime in detail.similar if anime.ref)
            entries = []
            seen = set(excluded)
            for anime in result.items:
                if not anime.ref or anime.ref in seen:
                    continue
                seen.add(anime.ref)
                entries.append(("recommendation", anime))
                if len(entries) == 6:
                    break
            if entries:
                should_expand = (
                    not self._related_button.isVisible() or self._related_button.isChecked()
                )
                self._add_related_carousel("Рекомендації на Mikai", entries)
                self._set_related_expanded(should_expand)

        def on_err(message: str) -> None:
            log.debug("recommendation carousel failed to load: %s", message)

        self._runner.run(lambda: browse(page=1, limit=18), on_ok, on_err)

    def _add_related_carousel(self, heading: str, entries: list[tuple[str, AnimeSummary]]) -> None:
        self._related_layout.addWidget(self._build_anime_carousel(heading, entries))

    def _set_related_expanded(self, expanded: bool) -> None:
        self._related_button.setVisible(expanded)
        self._related_host.setVisible(expanded)
        self._related_button.blockSignals(True)
        self._related_button.setChecked(expanded)
        self._related_button.blockSignals(False)
        self._related_button.setText("Сховати схожі" if expanded else "Пов’язані та схожі")

    def _toggle_description(self) -> None:
        self._description_expanded = not self._description_expanded
        self._description.setText(
            self._full_description if self._description_expanded else self._description_preview()
        )
        self._description_button.setText(
            "Згорнути опис" if self._description_expanded else "Читати далі"
        )

    def _description_preview(self) -> str:
        if len(self._full_description) <= TITLE_DESCRIPTION_PREVIEW_CHARS:
            return self._full_description
        excerpt = self._full_description[:TITLE_DESCRIPTION_PREVIEW_CHARS]
        excerpt = excerpt.rsplit(" ", 1)[0].rstrip(" ,.;:")
        return f"{excerpt}…"

    def _toggle_metadata(self, expanded: bool) -> None:
        self._meta_host.setVisible(expanded)
        self._meta_button.setText("Сховати подробиці" if expanded else "Докладніше про тайтл")
        if expanded:
            QTimer.singleShot(0, lambda: self._overview_scroll.ensureWidgetVisible(self._meta_host))

    def _related_host_visible(self, expanded: bool) -> None:
        self._related_host.setVisible(expanded)
        self._related_button.setText("Сховати схожі" if expanded else "Пов’язані та схожі")
        if expanded:
            QTimer.singleShot(
                0, lambda: self._overview_scroll.ensureWidgetVisible(self._related_host)
            )

    def _build_anime_carousel(
        self, heading: str, entries: list[tuple[str, AnimeSummary]]
    ) -> QWidget:
        section = QWidget()
        section_layout = QVBoxLayout(section)
        section_layout.setContentsMargins(0, 0, 0, 0)
        section_layout.setSpacing(6)

        header = QHBoxLayout()
        title = QLabel(heading)
        title.setObjectName("sectionHeading")
        title.setProperty("carouselHeading", True)
        header.addWidget(title)
        header.addStretch()
        previous = QPushButton("←")
        next_button = QPushButton("→")
        previous.setObjectName("carouselControl")
        next_button.setObjectName("carouselControl")
        header.addWidget(previous)
        header.addWidget(next_button)
        section_layout.addLayout(header)

        scroll = QScrollArea()
        scroll.setObjectName("animeCarousel")
        scroll.setWidgetResizable(False)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setMinimumHeight(CAROUSEL_CARD_HEIGHT + 12)
        track = QWidget()
        track.setObjectName("animeCarouselTrack")
        cards = QHBoxLayout(track)
        cards.setContentsMargins(2, 2, 2, 2)
        cards.setSpacing(10)

        for relation_kind, anime in entries:
            card = QPushButton()
            card.setObjectName("animeCard")
            card.setFixedSize(CAROUSEL_CARD_WIDTH, CAROUSEL_CARD_HEIGHT)
            card.setToolTip(anime.titles.display_name)
            card.clicked.connect(
                lambda _checked=False, ref=anime.ref, poster=anime.poster_url: (
                    self.related_open_requested.emit(ref, poster)
                )
            )

            card_layout = QVBoxLayout(card)
            card_layout.setContentsMargins(8, 8, 8, 8)
            card_layout.setSpacing(6)
            poster = QLabel("Постер недоступний")
            poster.setObjectName("carouselPoster")
            poster.setAlignment(Qt.AlignmentFlag.AlignCenter)
            poster.setFixedSize(CAROUSEL_POSTER_WIDTH, CAROUSEL_POSTER_HEIGHT)
            poster.setWordWrap(True)
            poster.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
            card_layout.addWidget(poster, alignment=Qt.AlignmentFlag.AlignHCenter)

            kind_label = RELATION_LABELS.get(
                relation_kind,
                RELATION_LABELS.get(
                    relation_kind.casefold(),
                    relation_kind.replace("_", " ").capitalize(),
                ),
            )
            kind = QLabel(kind_label)
            kind.setObjectName("carouselKind")
            kind.setAlignment(Qt.AlignmentFlag.AlignCenter)
            kind.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
            card_layout.addWidget(kind)

            name = QLabel(anime.titles.display_name)
            name.setObjectName("carouselTitle")
            name.setAlignment(Qt.AlignmentFlag.AlignCenter)
            name.setWordWrap(True)
            name.setFixedHeight(38)
            name.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
            card_layout.addWidget(name)
            cards.addWidget(card)

            if anime.poster_url:
                self._load_carousel_poster(anime.poster_url, poster)

        track.adjustSize()
        scroll.setWidget(track)
        section_layout.addWidget(scroll)
        bar = scroll.horizontalScrollBar()
        previous.clicked.connect(lambda: bar.setValue(bar.value() - CAROUSEL_SCROLL_STEP))
        next_button.clicked.connect(lambda: bar.setValue(bar.value() + CAROUSEL_SCROLL_STEP))
        previous.setEnabled(False)
        scroll.horizontalScrollBar().rangeChanged.connect(
            lambda minimum, maximum, left=previous, right=next_button: (
                left.setEnabled(maximum > minimum and bar.value() > minimum),
                right.setEnabled(bar.value() < maximum),
            )
        )
        scroll.horizontalScrollBar().valueChanged.connect(
            lambda value, minimum=bar.minimum, maximum=bar.maximum, left=previous, right=next_button: (
                left.setEnabled(value > minimum()),
                right.setEnabled(value < maximum()),
            )
        )
        return section

    def _load_carousel_poster(self, url: str, target: QLabel) -> None:
        def on_ok(data: bytes) -> None:
            if self._closed or not shiboken6.isValid(target):
                return
            pixmap = QPixmap()
            if pixmap.loadFromData(data) and not pixmap.isNull():
                target.setText("")
                target.setPixmap(
                    pixmap.scaled(
                        target.size(),
                        Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                        Qt.TransformationMode.SmoothTransformation,
                    )
                )

        def on_err(message: str) -> None:
            log.debug("related poster fetch failed for %s: %s", url, message)

        self._runner.run(lambda: self._repository.fetch_bytes(url), on_ok, on_err)

    @staticmethod
    def _scores_text(detail: AnimeDetail) -> str:
        parts = []
        scores = detail.scores
        if scores.mikai is not None:
            text = f"Mikai ★ {scores.mikai:g}"
            if scores.mikai_count:
                text += f" ({_compact_number(scores.mikai_count)})"
            parts.append(text)
        if scores.mal is not None:
            text = f"MAL {scores.mal:g}"
            if scores.mal_count:
                text += f" ({_compact_number(scores.mal_count)})"
            parts.append(text)
        return " · ".join(parts)

    def _release_label(self, release: Release, *, with_count: bool = True) -> str:
        kind = KIND_LABELS.get(release.kind, release.kind or "?")
        teams = ", ".join(team.name for team in release.teams if team.name)
        label = f"{kind} — {teams}" if teams else kind
        if release.is_collab:
            label += " (колаб)"
        if with_count:
            count = playable_episode_count(release)
            if count:
                label += f" · {count} серій"
        return label

    def _release_track_label(self, release: Release) -> str:
        """Track-picker label: release identity only, never the episode count.

        The count belongs to the whole release, but a track menu is scoped to the
        episode on screen, so "12 серій" there reads as the length of what is
        about to play and is simply wrong.
        """
        return self._release_label(release, with_count=False)

    def _fill_episodes(self, index: int) -> None:
        self._tabs.clear()
        self._url.clear()
        self._url.hide()
        self._copy_all.setEnabled(False)
        self._fetch_waiters.clear()
        self._tab_embeds = []
        self._tab_states = []
        if not 0 <= index < len(self._releases):
            return
        grouped: dict[str, list[tuple[str, str]]] = {}
        for episode in self._releases[index].episodes:
            number = episode.label or episode.number or "?"
            for source in episode.sources:
                if not source.embed_url:
                    continue
                try:
                    site = streams.detect_site(source.embed_url)
                except ValueError:
                    site = None
                provider = site or (source.provider.lower().strip() if source.provider else "other")
                grouped.setdefault(provider, []).append((number, source.embed_url))
        for provider in sorted(grouped):
            entries = grouped[provider]
            self._tab_embeds.append([embed_url for _, embed_url in entries])

            tab_page = QWidget()
            tab_layout = QVBoxLayout(tab_page)
            tab_layout.setContentsMargins(0, 0, 0, 0)
            tab_layout.setSpacing(6)

            scroll = QScrollArea()
            scroll.setObjectName("episodeScroll")
            scroll.setWidgetResizable(True)
            container = EpisodeGrid()
            container.layout_changed.connect(
                lambda: QTimer.singleShot(0, self._fit_episode_section)
            )
            scroll.setWidget(container)
            # the pager leads the section: page navigation sits at hand and the
            # grid cannot push it around as it grows
            pager = RangePager(EPISODES_PER_PAGE)
            tab_layout.addWidget(pager)
            tab_layout.addWidget(scroll, stretch=1)

            state = {
                "provider": provider,
                "entries": entries,
                "page": 0,
                "page_size": EPISODES_PER_PAGE,
                "grid": container,
                "pager": pager,
            }
            pager.pageRequested.connect(lambda page, s=state: self._go_to_episode_page(s, page))
            self._tab_states.append(state)

            self._tabs.addTab(tab_page, PROVIDER_LABELS.get(provider, provider.capitalize()))
            self._render_tab_page(state)

        self._copy_all.setEnabled(self._tabs.count() > 0)
        self._set_status("" if self._tabs.count() else "Немає джерел у релізі")
        self._fit_episode_section()

    def _fit_episode_section(self) -> None:
        if not hasattr(self, "_tabs"):
            return
        available_width = max(
            1,
            self._tabs.width()
            - 2 * self._tabs.style().pixelMetric(QStyle.PixelMetric.PM_DefaultFrameWidth),
        )
        for state in self._tab_states:
            grid = state["grid"]
            grid.set_layout_width(available_width)
        grid_heights = [state["grid"].sizeHint().height() for state in self._tab_states]
        pagination_heights = [
            state["pager"].sizeHint().height()
            for state in self._tab_states
            if not state["pager"].isHidden()
        ]
        grid_height = max(grid_heights, default=0)
        pagination_height = max(pagination_heights, default=0)
        tab_bar_height = self._tabs.tabBar().sizeHint().height() if self._tabs.count() else 0
        content_gap = 6 if pagination_height else 0
        frame = self._tabs.style().pixelMetric(QStyle.PixelMetric.PM_DefaultFrameWidth) * 2
        desired = tab_bar_height + grid_height + pagination_height + content_gap + frame
        # a 140-episode page would push the whole window; long lists scroll inside
        desired = max(76, min(desired, EPISODE_SECTION_MAX_HEIGHT))
        if self._tabs.height() != desired:
            self._tabs.setFixedHeight(desired)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        QTimer.singleShot(0, self._fit_episode_section)

    def _extract_active_tab(self) -> None:
        idx = self._tabs.currentIndex()
        if 0 <= idx < len(self._tab_states):
            state = self._tab_states[idx]
            for chip in state.get("visible_chips", []):
                self._request_chip_quality(chip)

    def _render_tab_page(self, state: dict) -> None:
        grid = state["grid"]

        page = state["page"]
        size = state["page_size"]
        total = len(state["entries"])
        total_pages = max(1, (total + size - 1) // size)

        start = page * size
        end = min(start + size, total)
        page_entries = state["entries"][start:end]

        entries = []
        for number, embed_url in page_entries:
            entries.append(self._build_episode_button(number, embed_url, state))
        grid.set_cards([entry["button"] for entry in entries])

        state["visible"] = entries
        pager = state["pager"]
        pager.setVisible(total_pages > 1)
        pager.set_state(page + 1, total_pages, len(state["entries"]))
        QTimer.singleShot(0, self._fit_episode_section)

    def _go_to_episode_page(self, state: dict, page: int) -> None:
        """Episodes are 0-based in the state and 1-based in the pager."""
        total_pages = max(1, (len(state["entries"]) + state["page_size"] - 1) // state["page_size"])
        index = page - 1
        if 0 <= index < total_pages and index != state["page"]:
            state["page"] = index
            self._render_tab_page(state)

    def _build_episode_button(self, number: str, embed_url: str, state: dict) -> dict:
        button = QPushButton(str(number))
        button.setObjectName("episodeButton")
        button.setFixedSize(EPISODE_BUTTON_WIDTH, EPISODE_BUTTON_HEIGHT)
        label = f"Серія {number}"
        button.setToolTip(f"{label} — дивитися")
        entry = {
            "button": button,
            "label": label,
            "number": str(number),
            "embed": embed_url,
            "state": state,
        }
        button.setProperty("embed", embed_url)
        button._entry = entry
        if embed_url in self._no_source_urls:
            self._mark_no_source(entry)
        button.clicked.connect(lambda _checked=False, e=entry: self._play_episode(e))
        button.setContextMenuPolicy(Qt.CustomContextMenu)
        button.customContextMenuRequested.connect(
            lambda pos, e=entry, host=button: self._episode_context_menu(e, host.mapToGlobal(pos))
        )
        return entry

    def _tab_changed(self) -> None:
        self._url.clear()
        self._url.hide()

    def _copy_episode(self, entry: dict) -> None:
        self._url.setText(entry["embed"])
        self._url.show()
        if not copy_text(entry["embed"]):
            self._flash_status("Не вдалося скопіювати URL серії")
            return
        if entry["embed"] in self._no_source_urls:
            self._flash_status("URL скопійовано (відео відсутнє у цього плеєра)")
        else:
            self._flash_status("URL скопійовано")

    def _play_episode(self, entry: dict) -> None:
        """The quality fan-out is gone: one episode fetches when it is opened."""
        self._ensure_qualities(entry, self._open_player)

    def _ensure_qualities(self, entry: dict, callback) -> None:
        """Fetch qualities for one embed once; concurrent callers share the flight."""
        if self._closed:
            return
        embed = entry["embed"]
        if embed in self._no_source_urls:
            self._flash_status("У цього плеєра немає відео для цієї серії")
            return
        cached = self._quality_cache.get(embed)
        if cached is not None:
            callback(entry, cached)
            return
        waiters = self._fetch_waiters.setdefault(embed, [])
        if any(existing is callback for _, existing in waiters):
            # a double click asks for the same thing twice; one flight is enough
            return
        if not waiters:
            self._flash_status("Завантаження якостей…")
            self._runner.run_bulk(
                lambda target=embed: streams.get_quality_streams(target),
                lambda qualities, target=embed: self._finish_fetch(target, qualities, None),
                lambda message, target=embed: self._finish_fetch(target, None, message),
            )
        waiters.append((entry, callback))

    def _finish_fetch(self, embed: str, qualities, message) -> None:
        waiters = self._fetch_waiters.pop(embed, [])
        if self._closed or not waiters:
            return
        if message is not None:
            log.warning("quality extraction failed for %s: %s", embed, message)
            if "no_source" in message.lower():
                self._no_source_urls.add(embed)
                for state in self._tab_states:
                    for entry in state.get("visible", []):
                        if entry["embed"] == embed:
                            self._mark_no_source(entry)
                self._flash_status("У цього плеєра немає відео для цієї серії")
            else:
                self._flash_status("Не вдалося завантажити якості серії")
            return
        self._remember_qualities(embed, qualities)
        for entry, callback in waiters:
            callback(entry, qualities)

    def _mark_no_source(self, entry: dict) -> None:
        button = entry.get("button")
        if button is None or not shiboken6.isValid(button):
            return
        button.setEnabled(False)
        button.setToolTip("У цього плеєра немає відео для цієї серії")

    def _remember_qualities(self, url: str, qualities: dict[int, str]) -> None:
        self._quality_cache[url] = dict(qualities)
        self._quality_fetched_at[url] = time.time()
        self._cache_save_timer.start()

    def _save_quality_cache(self) -> None:
        quality_cache.save(self._settings, self._quality_cache, self._quality_fetched_at)

    def _on_chip_quality_error(self, chip: dict, message: str) -> None:
        if self._closed:
            return
        state = chip.get("state", {})
        if chip.get("seq") != (state.get("extract_seq"), state.get("seq")):
            return
        layout = chip["holder"].layout()
        while layout.count():
            item = layout.takeAt(0)
            if item.widget() is not None:
                item.widget().deleteLater()
        if chip.pop("external_requested", False):
            self._flash_status("Не вдалося підготувати відео для зовнішнього плеєра")
        if "no_source" in message.lower():
            chip["no_source"] = True
            badge = QLabel("відео відсутнє")
            badge.setObjectName("muted")
            badge.setStyleSheet(
                f"color: {MUTED}; font-size: 11px; padding: 2px 6px; "
                f"border: 1px dashed {BORDER}; border-radius: 4px;"
            )
            badge.setToolTip(
                "У цього плеєра немає відео для цієї серії (no_source). "
                "Спробуйте іншу вкладку (Ashdi / Tortuga)."
            )
            layout.addWidget(badge)
        else:
            chip["requested"] = False
            retry = QPushButton("Повторити")
            retry.setObjectName("qualityRetry")
            retry.setToolTip(f"Якість тимчасово недоступна: {message}")
            retry.clicked.connect(
                lambda _checked=False, current=chip: self._request_chip_quality(current)
            )
            layout.addWidget(retry)

    def _on_chip_qualities(self, chip: dict, qualities: dict[int, str]) -> None:
        if self._closed:
            return
        state = chip.get("state", {})
        if chip.get("seq") != (state.get("extract_seq"), state.get("seq")):
            return
        chip["qualities"] = dict(qualities)
        layout = chip["holder"].layout()
        while layout.count():
            item = layout.takeAt(0)
            if item.widget() is not None:
                item.widget().deleteLater()
        for height in sorted(qualities, reverse=True):
            stream_url = qualities[height]
            if not stream_url or not isinstance(stream_url, str):
                continue
            button = QPushButton(f"{height}p")
            button.setObjectName("qualityChip")
            button.setContextMenuPolicy(Qt.CustomContextMenu)
            button.customContextMenuRequested.connect(
                lambda pos, h=height, u=stream_url, c=chip, host=button: (
                    self._quality_chip_context_menu(c, h, u, host.mapToGlobal(pos))
                )
            )
            button.clicked.connect(
                lambda _checked=False, h=height, u=stream_url, c=chip: self._quality_clicked(
                    c, h, u
                )
            )
            layout.addWidget(button)
        if not layout.count():
            unavailable = QLabel("Якість недоступна")
            unavailable.setObjectName("muted")
            layout.addWidget(unavailable)
        if chip.pop("external_requested", False):
            self._open_external_stream(chip, qualities)
        # the holder's width decides the grid's columns, so the section refits itself
        QTimer.singleShot(0, self._fit_episode_section)

    def _quality_clicked(self, chip: dict, height: int, url: str) -> None:
        # the player opens either way: the copy is a convenience, not a precondition
        if copy_text(url):
            self._flash_status(f"Скопійовано {height}p")
        else:
            self._flash_status(f"Не вдалося скопіювати {height}p")
        self._open_stream(chip, height, url)

    def _recover_stream(self, dialog: PlayerDialog) -> None:
        """Signed stream URLs expire, so re-extract once before reporting failure.

        The re-extraction targets whatever track the dialog is on RIGHT NOW: a
        dub/sub switch replaces the record, so recovery must not rewind the
        episode to the variant it opened with.
        """
        if self._closed or not shiboken6.isValid(dialog):
            return
        entry = getattr(dialog, "_page_entry", None)
        if entry is None:
            return
        embed = entry["embed"]
        height = dialog.controller.state.quality
        self._quality_cache.pop(embed, None)
        self._quality_fetched_at.pop(embed, None)
        self._no_source_urls.discard(embed)
        self._flash_status("Потік застарів — оновлюю посилання…")

        def on_ok(qualities: dict[int, str], target: PlayerDialog = dialog) -> None:
            if not qualities:
                return
            self._remember_qualities(embed, qualities)
            if shiboken6.isValid(target):
                target.apply_refreshed_streams(qualities, height)

        def on_err(message: str) -> None:
            log.warning("stream re-extraction failed for %s: %s", embed, message)

        self._runner.run(lambda: streams.get_quality_streams(embed), on_ok, on_err)

    def _episode_context_menu(self, chip: dict, global_pos) -> None:
        menu = self._build_episode_menu(chip)
        menu.exec(global_pos)

    def _build_episode_menu(self, entry: dict) -> QMenu:
        menu = QMenu(self)
        copy_action = menu.addAction("Копіювати в буфер обміну")
        copy_action.setToolTip("Скопіювати URL серії")
        copy_action.triggered.connect(lambda _=False, e=entry: self._copy_episode(e))
        menu.addSeparator()
        external_action = menu.addAction("Відкрити у зовнішньому плеєрі")
        external_action.setToolTip("Відкрити найкращу якість у mpv або VLC")
        external_action.setEnabled(entry["embed"] not in self._no_source_urls)
        external_action.triggered.connect(lambda _=False, e=entry: self._open_external_player(e))
        open_action = menu.addAction("Відкрити в браузері")
        open_action.setToolTip("Відкрити сторінку серії")
        open_action.triggered.connect(lambda _=False, e=entry: self._open_embed_in_browser(e))
        return menu

    def _episode_variants(self, number: str) -> list[dict]:
        """Every dub/sub track carrying this episode, across releases and players."""
        variants: list[dict] = []
        seen: set[str] = set()
        for release in self._releases:
            for episode in release.episodes:
                if (episode.label or episode.number or "?") != number:
                    continue
                for source in episode.sources:
                    embed = source.embed_url
                    if not embed or embed in seen:
                        continue
                    seen.add(embed)
                    try:
                        site = streams.detect_site(embed)
                    except ValueError:
                        site = None
                    provider = site or (
                        source.provider.lower().strip() if source.provider else "other"
                    )
                    track = PROVIDER_LABELS.get(provider, provider.capitalize())
                    variants.append(
                        {"label": f"{self._release_track_label(release)} · {track}", "embed": embed}
                    )
        return variants

    def _tab_playlist(self, state: dict | None, number: str) -> tuple[list[dict], int]:
        """The episodes of the release tab being watched, for the player sidebar."""
        if not state:
            return [], 0
        entries = state.get("entries", [])
        playlist: list[dict] = []
        index = 0
        for spot, (item_number, embed) in enumerate(entries):
            playlist.append({"label": f"Серія {item_number}", "embed": embed})
            if item_number == number:
                index = spot
        return playlist, index

    def _open_player(self, entry: dict, qualities: dict[int, str]) -> None:
        available = [
            (height, url)
            for height, url in qualities.items()
            if isinstance(url, str) and url.strip()
        ]
        if not available:
            self._flash_status("Не вдалося знайти потік для серії")
            return
        height, url = max(available, key=lambda item: item[0])
        caption = f"{entry['label']} · {height}p"
        playlist, index = self._tab_playlist(entry.get("state"), entry["number"])
        dialog = PlayerDialog(
            caption,
            url,
            qualities=dict(qualities),
            initial_height=height,
            variants=self._episode_variants(entry["number"]),
            current_embed=entry["embed"],
            playlist=playlist,
            episode_index=index,
            parent=self,
        )
        # recovery must re-extract whatever track is CURRENT, not the opening one
        dialog._page_entry = entry
        self._player_dialogs.append(dialog)
        dialog.runtimeRequested.connect(lambda target=dialog: self._prepare_player(target))
        dialog.playbackFailed.connect(lambda target=dialog: self._recover_stream(target))
        dialog.variantRequested.connect(
            lambda embed, label, target=dialog: self._switch_player_variant(target, embed, label)
        )
        dialog.episodeRequested.connect(
            lambda item, spot, target=dialog: self._play_episode_from(target, item, spot)
        )
        dialog.show()
        self._prepare_player(dialog)

    def _play_episode_from(self, dialog: PlayerDialog, item: dict, index: int) -> None:
        """Step inside the open player: one fetch, then swap in place."""
        if self._closed or not shiboken6.isValid(dialog):
            return
        number = str(item.get("label", "")).removeprefix("Серія ")
        variants = self._episode_variants(number)
        entry = {
            "button": None,
            "label": item.get("label", ""),
            "number": number,
            "embed": item.get("embed", ""),
            "state": None,
        }

        def on_qualities(loaded, qualities) -> None:
            if not shiboken6.isValid(dialog):
                return
            dialog._page_entry = loaded
            dialog.apply_episode(loaded["label"], loaded["embed"], index, qualities, variants)

        self._ensure_qualities(entry, on_qualities)

    def _switch_player_variant(self, dialog: PlayerDialog, embed: str, label: str) -> None:
        if self._closed or not shiboken6.isValid(dialog):
            return
        entry = {"button": None, "label": label, "embed": embed, "state": None}

        def on_qualities(loaded, qualities) -> None:
            if not shiboken6.isValid(dialog):
                return
            dialog._page_entry = loaded
            dialog.apply_variant(qualities, loaded["label"], loaded["embed"])

        self._ensure_qualities(entry, on_qualities)

    def _open_external_player(self, entry: dict) -> None:
        self._ensure_qualities(entry, self._open_external_stream)

    def _open_external_stream(self, entry: dict, qualities: dict[int, str]) -> None:
        available = [
            (height, url)
            for height, url in qualities.items()
            if isinstance(url, str) and url.strip()
        ]
        if not available:
            self._flash_status("Не знайдено доступної якості відео")
            return

        height, stream_url = max(available, key=lambda item: item[0])
        external_player = self._find_external_player()
        if external_player is None:
            self._flash_status("Зовнішній плеєр не знайдено — встановіть mpv або VLC")
            return
        program, player = external_player
        arguments = [stream_url] if player == "mpv" else ["--play-and-exit", stream_url]
        if self._start_external_process(program, arguments):
            self._flash_status(f"Відкрито у {player} · {height}p")
        else:
            self._flash_status(f"Не вдалося запустити {player}")

    @staticmethod
    def _find_external_player() -> tuple[str, str] | None:
        for executable in ("mpv", "vlc"):
            program = QStandardPaths.findExecutable(executable)
            if program:
                return program, executable
        return None

    @staticmethod
    def _start_external_process(program: str, arguments: list[str]) -> bool:
        result = QProcess.startDetached(program, arguments)
        return result[0] if isinstance(result, tuple) else bool(result)

    def _copy_quality_url(self, height: int, url: str) -> None:
        if copy_text(url):
            self._flash_status(f"Скопійовано {height}p")
        else:
            self._flash_status(f"Не вдалося скопіювати {height}p")

    def _open_embed_in_browser(self, chip: dict) -> None:
        QDesktopServices.openUrl(QUrl(chip["embed"]))
        self._flash_status("Відкрито в браузері")

    def _prepare_player(self, dialog: PlayerDialog) -> None:
        def on_ok(runtime_dir: Path, target: PlayerDialog = dialog) -> None:
            if shiboken6.isValid(target):
                target.start_player(runtime_dir)

        def on_err(message: str, target: PlayerDialog = dialog) -> None:
            if shiboken6.isValid(target):
                target.show_error(message)

        self._runner.run(prepare_runtime, on_ok, on_err)

    def shutdown(self) -> None:
        self._closed = True
        self._cache_save_timer.stop()
        if self._quality_cache:
            self._save_quality_cache()
        self._fetch_waiters.clear()
        for dialog in self._player_dialogs:
            if shiboken6.isValid(dialog):
                dialog.close()
        self._player_dialogs.clear()
        if self._cover_viewer is not None and shiboken6.isValid(self._cover_viewer):
            self._cover_viewer.close()
        self._cover_viewer = None

    def _set_status(self, text: str) -> None:
        self._base_status = text
        self._status.setText(text)

    def _flash_status(self, text: str) -> None:
        self._status.setText(text)
        QTimer.singleShot(
            1200,
            lambda: (
                self._status.setText(self._base_status)
                if not self._closed and shiboken6.isValid(self._status)
                else None
            ),
        )

    def _copy_all_urls(self) -> None:
        index = self._tabs.currentIndex()
        embeds = self._tab_embeds[index] if 0 <= index < len(self._tab_embeds) else []
        urls = [url for url in embeds if url]
        if not urls:
            return
        if copy_text("\n".join(urls)):
            self._flash(self._copy_all, "Копіювати всі", f"Скопійовано ({len(urls)})")
        else:
            self._flash(self._copy_all, "Копіювати всі", "Не вдалося скопіювати")

    def _flash(self, button: QPushButton, original: str, done: str) -> None:
        button.setText(done)
        QTimer.singleShot(
            1200, lambda: button.setText(original) if shiboken6.isValid(button) else None
        )

    def _load_cover(self, url: str, allow_hint: bool) -> None:
        def on_ok(data: bytes, loaded_url: str = url) -> None:
            if self._closed or not shiboken6.isValid(self):
                return
            pixmap = QPixmap()
            if pixmap.loadFromData(data) and not pixmap.isNull():
                self._cover_pixmap = pixmap
                self._cover_url = loaded_url
                self._cover.set_zoomable(True)
                self._cover.setToolTip("Натисніть, щоб відкрити обкладинку")
                self._cover.setText("")
                self._cover.setPixmap(
                    pixmap.scaled(
                        self._cover.size(),
                        Qt.AspectRatioMode.KeepAspectRatio,
                        Qt.TransformationMode.SmoothTransformation,
                    )
                )

        def on_err(message: str) -> None:
            if self._closed or not shiboken6.isValid(self):
                return
            if allow_hint and self._poster_hint and self._poster_hint != url:
                self._load_cover(self._poster_hint, allow_hint=False)
            else:
                self._cover.set_zoomable(False)
                self._cover.setText("Без обкладинки")

        self._runner.run(lambda: self._repository.fetch_bytes(url), on_ok, on_err)

    def _cover_context_menu(self, global_pos) -> None:
        menu = QMenu(self)
        copy_url = menu.addAction("Копіювати URL обкладинки")
        copy_url.setToolTip("Скопіювати адресу зображення")
        copy_url.setEnabled(bool(self._cover_url))
        copy_url.triggered.connect(lambda _=False: self._copy_cover_url())
        save_action = menu.addAction("Зберегти обкладинку…")
        save_action.setToolTip("Зберегти зображення у файл")
        save_action.setEnabled(not self._cover_pixmap.isNull())
        save_action.triggered.connect(lambda _=False: self._save_cover())
        menu.exec(global_pos)

    def _copy_cover_url(self) -> None:
        if not self._cover_url:
            return
        if not copy_text(self._cover_url):
            self._flash_status("Не вдалося скопіювати URL обкладинки")
            return
        self._flash_status("URL обкладинки скопійовано")

    def _save_cover(self) -> None:
        if self._cover_pixmap.isNull():
            return
        from PySide6.QtWidgets import QFileDialog

        suffix = "webp" if self._cover_url.endswith(".webp") else "png"
        path, _ = QFileDialog.getSaveFileName(
            self, "Зберегти обкладинку", f"cover.{suffix}", "Зображення (*.png *.jpg *.webp)"
        )
        if not path:
            return
        if self._cover_pixmap.save(path):
            self._flash_status(f"Обкладинку збережено: {path}")
        else:
            self._flash_status("Не вдалося зберегти обкладинку")

    def _open_cover_viewer(self) -> None:
        if self._cover_pixmap.isNull():
            return
        if self._cover_viewer is not None and shiboken6.isValid(self._cover_viewer):
            self._cover_viewer.close()
        viewer = CoverViewer(self._cover_pixmap, self._heading.text(), self)
        viewer.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        # The viewer deletes itself on close, so drop the reference with it.
        viewer.destroyed.connect(lambda *_args: setattr(self, "_cover_viewer", None))
        self._cover_viewer = viewer
        viewer.show()

    def _on_error(self, message: str) -> None:
        if self._closed or not shiboken6.isValid(self):
            return
        self._rate_label.refresh()
        self._set_status(message)
        self._cover.set_zoomable(False)
        self._cover.setText("Без обкладинки")
