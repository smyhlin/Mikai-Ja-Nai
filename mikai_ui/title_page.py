from __future__ import annotations

import logging
from pathlib import Path

import shiboken6
from PySide6.QtCore import QProcess, QSize, QStandardPaths, Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QDesktopServices, QGuiApplication, QPixmap
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
from mikai_backend.releases import playable_episode_count, sort_releases
from mikai_ui.mpv_backend import prepare_runtime
from mikai_ui.player_dialog import PlayerDialog
from mikai_ui.rate_limit_label import RateLimitLabel
from mikai_ui.settings_dialog import make_gear_icon
from mikai_ui.theme import ACCENT, BORDER, FG, MUTED
from mikai_ui.theme.metrics import (
    CAROUSEL_CARD_HEIGHT,
    CAROUSEL_CARD_WIDTH,
    CAROUSEL_POSTER_HEIGHT,
    CAROUSEL_POSTER_WIDTH,
    CAROUSEL_SCROLL_STEP,
    EPISODE_CARD_MIN_HEIGHT,
    EPISODE_CARD_MIN_WIDTH,
    TITLE_COVER_HEIGHT,
    TITLE_COVER_WIDTH,
    TITLE_DESCRIPTION_PREVIEW_CHARS,
)
from mikai_ui.workers import Runner

log = logging.getLogger(__name__)

KIND_LABELS = {"voice": "Озвучення", "sub": "Субтитри"}
PROVIDER_LABELS = {"ashdi": "Ashdi", "moon": "MoonAnime", "tortuga": "Tortuga"}
EPISODES_PER_PAGE = 12
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
STATUS_LABELS = {
    "finished": "Закінчене",
    "ongoing": "Триває",
    "announce": "Анонсовано",
    "cancelled": "Скасовано",
    "break": "Перерва",
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
        self._grid.setContentsMargins(8, 8, 8, 8)
        self._grid.setSpacing(8)
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
        columns = max(1, min(4, self.width() // EPISODE_CARD_MIN_WIDTH))
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
        self._extract_seq = 0
        self._tab_embeds: list[list[str]] = []
        self._tab_states: list[dict] = []
        self._player_dialogs: list[PlayerDialog] = []
        self._quality_queue: list[dict] = []
        self._quality_in_progress = False
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
        self._settings_btn.setIcon(make_gear_icon(18, FG))
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

        self._cover = QLabel("Завантаження…")
        self._cover.setObjectName("titleCover")
        self._cover.setFixedSize(TITLE_COVER_WIDTH, TITLE_COVER_HEIGHT)
        self._cover.setAlignment(Qt.AlignmentFlag.AlignCenter)
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
        episodes_heading = QLabel("Серії та якість")
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

    def _release_label(self, release: Release) -> str:
        kind = KIND_LABELS.get(release.kind, release.kind or "?")
        teams = ", ".join(team.name for team in release.teams if team.name)
        label = f"{kind} — {teams}" if teams else kind
        if release.is_collab:
            label += " (колаб)"
        count = playable_episode_count(release)
        if count:
            label += f" · {count} серій"
        return label

    def _fill_episodes(self, index: int) -> None:
        self._tabs.clear()
        self._url.clear()
        self._url.hide()
        self._copy_all.setEnabled(False)
        self._extract_seq += 1
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
                grouped.setdefault(provider, []).append((f"Серія {number}", source.embed_url))
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
            tab_layout.addWidget(scroll, stretch=1)

            pagination_bar = QWidget()
            pag_layout = QHBoxLayout(pagination_bar)
            pag_layout.setContentsMargins(8, 4, 8, 4)
            pag_layout.setSpacing(12)
            prev_btn = QPushButton("←")
            prev_btn.setFixedWidth(50)
            page_label = QLabel("")
            page_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
            next_btn = QPushButton("→")
            next_btn.setFixedWidth(50)
            pag_layout.addStretch()
            pag_layout.addWidget(prev_btn)
            pag_layout.addWidget(page_label)
            pag_layout.addWidget(next_btn)
            pag_layout.addStretch()
            tab_layout.addWidget(pagination_bar)

            state = {
                "provider": provider,
                "entries": entries,
                "page": 0,
                "page_size": EPISODES_PER_PAGE,
                "grid": container,
                "page_label": page_label,
                "prev_btn": prev_btn,
                "next_btn": next_btn,
                "pagination_bar": pagination_bar,
                "seq": 0,
                "extract_seq": self._extract_seq,
            }
            self._tab_states.append(state)

            prev_btn.clicked.connect(lambda _=False, s=state: self._change_page(s, -1))
            next_btn.clicked.connect(lambda _=False, s=state: self._change_page(s, 1))

            self._tabs.addTab(tab_page, PROVIDER_LABELS.get(provider, provider.capitalize()))
            self._render_tab_page(state)

        self._copy_all.setEnabled(self._tabs.count() > 0)
        self._set_status("" if self._tabs.count() else "Немає джерел у релізі")
        self._extract_active_tab()
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
            state["pagination_bar"].sizeHint().height()
            for state in self._tab_states
            if not state["pagination_bar"].isHidden()
        ]
        grid_height = max(grid_heights, default=0)
        pagination_height = max(pagination_heights, default=0)
        tab_bar_height = self._tabs.tabBar().sizeHint().height() if self._tabs.count() else 0
        content_gap = 6 if pagination_height else 0
        frame = self._tabs.style().pixelMetric(QStyle.PixelMetric.PM_DefaultFrameWidth) * 2
        desired = max(76, tab_bar_height + grid_height + pagination_height + content_gap + frame)
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
        self._drain_quality_queue()

    def _render_tab_page(self, state: dict) -> None:
        state["seq"] += 1
        grid = state["grid"]

        page = state["page"]
        size = state["page_size"]
        total = len(state["entries"])
        total_pages = max(1, (total + size - 1) // size)

        start = page * size
        end = min(start + size, total)
        page_entries = state["entries"][start:end]

        chips = []
        for text, embed_url in page_entries:
            chip = self._build_chip(text, embed_url, state)
            chips.append(chip)
        grid.set_cards([chip["widget"] for chip in chips])

        state["visible_chips"] = chips
        state["page_label"].setText(f"{page + 1} / {total_pages}")
        state["prev_btn"].setEnabled(page > 0)
        state["next_btn"].setEnabled(page < total_pages - 1)
        state["pagination_bar"].setVisible(total_pages > 1)
        QTimer.singleShot(0, self._fit_episode_section)

        # Only extract if this tab is currently the active tab
        if self._tab_states and self._tab_states.index(state) == self._tabs.currentIndex():
            for chip in chips:
                self._request_chip_quality(chip)

    def _change_page(self, state: dict, delta: int) -> None:
        new_page = state["page"] + delta
        total_pages = max(1, (len(state["entries"]) + state["page_size"] - 1) // state["page_size"])
        if 0 <= new_page < total_pages:
            state["page"] = new_page
            self._url.clear()
            self._url.hide()
            self._render_tab_page(state)

    def _build_chip(self, text: str, embed_url: str, state: dict) -> dict:
        widget = QWidget()
        widget.setObjectName("episodeCard")
        widget.setProperty("embed", embed_url)
        widget.setMinimumHeight(EPISODE_CARD_MIN_HEIGHT)
        widget.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        row = QHBoxLayout(widget)
        row.setContentsMargins(12, 8, 12, 8)
        row.setSpacing(8)
        chip = {
            "widget": widget,
            "text": text,
            "embed": embed_url,
            "holder": QWidget(),
            "requested": False,
            "state": state,
            "seq": (state["extract_seq"], state["seq"]),
        }
        chip["holder"].setObjectName("qualityHolder")
        widget._chip = chip
        widget.setContextMenuPolicy(Qt.CustomContextMenu)
        widget.customContextMenuRequested.connect(
            lambda pos, current=chip, host=widget: self._episode_context_menu(
                current, host.mapToGlobal(pos)
            )
        )
        episode = QPushButton(text)
        episode.setObjectName("episodeCopy")
        episode.setToolTip("Копіювати URL серії")
        episode.clicked.connect(lambda _checked=False, current=chip: self._copy_episode(current))
        episode.setContextMenuPolicy(Qt.CustomContextMenu)
        episode.customContextMenuRequested.connect(
            lambda pos, current=chip, host=episode: self._episode_context_menu(
                current, host.mapToGlobal(pos)
            )
        )
        row.addWidget(episode)
        row.addStretch()
        holder_layout = QHBoxLayout(chip["holder"])
        holder_layout.setContentsMargins(0, 0, 0, 0)
        holder_layout.setSpacing(4)
        loading = QLabel("Якість…")
        loading.setObjectName("muted")
        holder_layout.addWidget(loading)
        row.addWidget(chip["holder"])
        return chip

    def _tab_changed(self) -> None:
        self._url.clear()
        self._url.hide()
        self._extract_active_tab()

    def _copy_episode(self, chip: dict) -> None:
        self._url.setText(chip["embed"])
        self._url.show()
        QGuiApplication.clipboard().setText(chip["embed"])
        if chip.get("no_source"):
            self._flash_status("URL скопійовано (відео відсутнє у цього плеєра)")
        else:
            self._flash_status("URL скопійовано")
        self._request_chip_quality(chip)

    def _request_chip_quality(self, chip: dict) -> None:
        if self._closed:
            return
        if chip["requested"]:
            return
        chip["requested"] = True
        holder_layout = chip["holder"].layout()
        while holder_layout.count():
            item = holder_layout.takeAt(0)
            if item.widget() is not None:
                item.widget().deleteLater()
        loading = QLabel("Якість…")
        loading.setObjectName("muted")
        holder_layout.addWidget(loading)
        state = chip["state"]
        chip["seq"] = (state["extract_seq"], state["seq"])
        self._quality_queue.append(chip)
        self._drain_quality_queue()

    def _drain_quality_queue(self) -> None:
        if self._closed or self._quality_in_progress:
            return
        self._quality_queue = [
            chip
            for chip in self._quality_queue
            if chip.get("seq")
            == (
                chip.get("state", {}).get("extract_seq"),
                chip.get("state", {}).get("seq"),
            )
        ]
        active_index = next(
            (
                index
                for index, chip in enumerate(self._quality_queue)
                if chip.get("state") in self._tab_states
                and self._tab_states.index(chip["state"]) == self._tabs.currentIndex()
            ),
            None,
        )
        if active_index is not None:
            chip = self._quality_queue.pop(active_index)
            self._quality_in_progress = True
            url = chip["embed"]

            def on_ok(qualities: dict[int, str], current=chip) -> None:
                self._on_chip_qualities(current, qualities)
                self._quality_in_progress = False
                self._drain_quality_queue()

            def on_err(message: str, current=chip, target_url=url) -> None:
                log.warning("quality extraction failed for %s: %s", target_url, message)
                self._on_chip_quality_error(current, message)
                self._quality_in_progress = False
                self._drain_quality_queue()

            self._runner.run(lambda target=url: streams.get_quality_streams(target), on_ok, on_err)

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

    def _quality_clicked(self, chip: dict, height: int, url: str) -> None:
        QGuiApplication.clipboard().setText(url)
        self._flash_status(f"Скопійовано {height}p")
        caption = f"{chip.get('text', '')} · {height}p"
        dialog = PlayerDialog(
            caption,
            url,
            qualities=chip.get("qualities"),
            initial_height=height,
            parent=self,
        )
        self._player_dialogs.append(dialog)
        dialog.runtimeRequested.connect(lambda target=dialog: self._prepare_player(target))
        dialog.show()
        self._prepare_player(dialog)

    def _episode_context_menu(self, chip: dict, global_pos) -> None:
        menu = self._build_episode_menu(chip)
        menu.exec(global_pos)

    def _build_episode_menu(self, chip: dict) -> QMenu:
        menu = QMenu(self)
        copy_action = menu.addAction("Копіювати в буфер обміну")
        copy_action.triggered.connect(lambda _=False, c=chip: self._copy_episode(c))

        quality_menu = menu.addMenu("Копіювати якість")
        qualities = chip.get("qualities") or {}
        if qualities:
            for height in sorted(qualities, reverse=True):
                url = qualities[height]
                action = quality_menu.addAction(f"{height}p")
                action.triggered.connect(
                    lambda _=False, h=height, u=url: self._copy_quality_url(h, u)
                )
        elif chip.get("no_source"):
            unavailable = quality_menu.addAction("Відео відсутнє у цього плеєра")
            unavailable.setEnabled(False)
        else:
            pending = quality_menu.addAction("Ще не завантажено")
            pending.setEnabled(False)
            self._request_chip_quality(chip)

        menu.addSeparator()
        external_action = menu.addAction("Відкрити у зовнішньому плеєрі")
        external_action.setEnabled(not chip.get("no_source"))
        external_action.triggered.connect(lambda _=False, c=chip: self._open_external_player(c))
        open_action = menu.addAction("Відкрити в браузері")
        open_action.triggered.connect(lambda _=False, c=chip: self._open_embed_in_browser(c))
        return menu

    def _open_external_player(self, chip: dict) -> None:
        qualities = chip.get("qualities") or {}
        if qualities:
            self._open_external_stream(chip, qualities)
            return
        if chip.get("no_source"):
            self._flash_status("У цього плеєра немає відео для серії")
            return
        chip["external_requested"] = True
        if not chip.get("requested"):
            self._request_chip_quality(chip)
        self._flash_status("Підготовка потоку для зовнішнього плеєра…")

    def _open_external_stream(self, chip: dict, qualities: dict[int, str]) -> None:
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
        QGuiApplication.clipboard().setText(url)
        self._flash_status(f"Скопійовано {height}p")

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
        self._extract_seq += 1
        for dialog in self._player_dialogs:
            if shiboken6.isValid(dialog):
                dialog.close()
        self._player_dialogs.clear()

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
        if urls:
            QGuiApplication.clipboard().setText("\n".join(urls))
            self._flash(self._copy_all, "Копіювати всі", f"Скопійовано ({len(urls)})")

    def _flash(self, button: QPushButton, original: str, done: str) -> None:
        button.setText(done)
        QTimer.singleShot(
            1200, lambda: button.setText(original) if shiboken6.isValid(button) else None
        )

    def _load_cover(self, url: str, allow_hint: bool) -> None:
        def on_ok(data: bytes) -> None:
            if self._closed or not shiboken6.isValid(self):
                return
            pixmap = QPixmap()
            if pixmap.loadFromData(data) and not pixmap.isNull():
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
                self._cover.setText("Без обкладинки")

        self._runner.run(lambda: self._repository.fetch_bytes(url), on_ok, on_err)

    def _on_error(self, message: str) -> None:
        if self._closed or not shiboken6.isValid(self):
            return
        self._rate_label.refresh()
        self._set_status(message)
        self._cover.setText("Без обкладинки")
