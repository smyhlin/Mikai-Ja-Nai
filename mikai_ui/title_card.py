"""Home-feed title card: poster, title, airing status and episode count."""

from __future__ import annotations

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import QHBoxLayout, QLabel, QPushButton, QVBoxLayout

from mikai_backend import AnimeDetail, AnimeSummary
from mikai_ui.labels import STATUS_LABELS
from mikai_ui.theme.metrics import (
    HOME_CARD_HEIGHT,
    HOME_CARD_WIDTH,
    HOME_POSTER_HEIGHT,
    HOME_POSTER_WIDTH,
    SPACING_LARGE,
    SPACING_MEDIUM,
    SPACING_XS,
)

PENDING = "…"


def episode_count_text(aired: int, total: int | None) -> str:
    if aired and total:
        return f"{aired} / {total} серій"
    if aired:
        return f"{aired} серій"
    return f"{total} серій" if total else ""


class TitleCard(QPushButton):
    """One entry of a home carousel. List responses carry the planned episode count
    only, so the aired fraction stays a placeholder until a detail response lands."""

    openRequested = Signal(str, object)
    menuRequested = Signal(str, object)

    def __init__(self, summary: AnimeSummary, parent=None) -> None:
        super().__init__(parent)
        self.ref = summary.ref
        self.image_key = summary.thumb_url or summary.poster_url or ""
        self._poster_url = summary.poster_url or ""
        self._planned = summary.episodes
        self.setObjectName("animeCard")
        self.setFixedSize(HOME_CARD_WIDTH, HOME_CARD_HEIGHT)
        self.setToolTip(summary.titles.display_name)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)

        row = QHBoxLayout(self)
        row.setContentsMargins(SPACING_MEDIUM, SPACING_MEDIUM, SPACING_LARGE, SPACING_MEDIUM)
        row.setSpacing(SPACING_MEDIUM)
        self.poster = self._label("carouselPoster", "Постер недоступний")
        self.poster.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.poster.setFixedSize(HOME_POSTER_WIDTH, HOME_POSTER_HEIGHT)
        self.poster.setWordWrap(True)
        row.addWidget(self.poster, alignment=Qt.AlignmentFlag.AlignTop)

        text = QVBoxLayout()
        text.setContentsMargins(0, 0, 0, 0)
        text.setSpacing(SPACING_XS)
        self.title = self._label("carouselTitle", summary.titles.display_name)
        self.title.setWordWrap(True)
        self.title.setFixedHeight(self.title.fontMetrics().height() * 2)
        self.status = self._label("carouselStatus", "")
        self.episodes = self._label("carouselEpisodes", "")
        for label in (self.title, self.status, self.episodes):
            text.addWidget(label)
        text.addStretch(1)
        row.addLayout(text, stretch=1)

        self.clicked.connect(lambda: self.openRequested.emit(self.ref, self._poster_url))
        self.customContextMenuRequested.connect(
            lambda pos: self.menuRequested.emit(self.ref, self.mapToGlobal(pos))
        )
        self.apply_status(summary.status)
        self.apply_episodes(0, summary.episodes)

    def _label(self, object_name: str, text: str) -> QLabel:
        label = QLabel(text, self)
        label.setObjectName(object_name)
        label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        return label

    def apply_pixmap(self, pixmap: QPixmap) -> None:
        self.poster.setText("")
        self.poster.setPixmap(
            pixmap.scaled(
                self.poster.size(),
                Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                Qt.TransformationMode.SmoothTransformation,
            )
        )

    def apply_status(self, status: str) -> None:
        self.status.setObjectName("carouselStatusLive" if status == "ongoing" else "carouselStatus")
        self.status.setText(STATUS_LABELS.get(status, "") or PENDING)

    def apply_episodes(self, aired: int, total: int | None) -> None:
        self._planned = total or self._planned
        self.episodes.setText(episode_count_text(aired, self._planned) or PENDING)

    def apply_detail(self, detail: AnimeDetail) -> None:
        self.apply_status(detail.status)
        self.apply_episodes(detail.episodes.aired, detail.episodes.total)
