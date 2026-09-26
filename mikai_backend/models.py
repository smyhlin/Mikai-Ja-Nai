from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(frozen=True)
class Titles:
    ua: str | None = None
    english: str | None = None
    original: str | None = None

    @property
    def display_name(self) -> str:
        return self.ua or self.english or self.original or "?"


@dataclass(frozen=True)
class ImageVariant:
    webp: str | None = None
    jpg: str | None = None


@dataclass(frozen=True)
class PosterSet:
    big: ImageVariant = field(default_factory=ImageVariant)
    medium: ImageVariant = field(default_factory=ImageVariant)
    small: ImageVariant = field(default_factory=ImageVariant)


@dataclass(frozen=True)
class Team:
    id: int = 0
    slug: str = ""
    name: str = ""


@dataclass(frozen=True)
class Source:
    provider: str = "?"
    embed_url: str = ""


@dataclass(frozen=True)
class Episode:
    number: int | float | None = None
    label: str | None = None
    sources: list[Source] = field(default_factory=list)


@dataclass(frozen=True)
class Release:
    id: str = ""
    kind: str = ""
    is_collab: bool = False
    teams: list[Team] = field(default_factory=list)
    episodes_count: int | None = None
    episodes: list[Episode] = field(default_factory=list)


@dataclass(frozen=True)
class UaPoster:
    images: PosterSet = field(default_factory=PosterSet)


@dataclass(frozen=True)
class GenreTag:
    name: str = ""
    ua: str | None = None

    @property
    def display(self) -> str:
        return self.ua or self.name


@dataclass(frozen=True)
class EpisodeStats:
    total: int | None = None
    aired: int = 0
    localized: int = 0
    duration_min: int | None = None


@dataclass(frozen=True)
class Scores:
    mikai: float | None = None
    mikai_count: int = 0
    mal: float | None = None
    mal_count: int = 0


MIKAI_PAGE_URL = "https://mikai.me/anime/"


def anime_page_url(ref: str) -> str:
    """Public website page for a title ref, bare Mikai id or prefixed form."""
    return f"{MIKAI_PAGE_URL}{ref}"


@dataclass(frozen=True)
class AnimeSummary:
    ref: str = ""
    titles: Titles = field(default_factory=Titles)
    year: int | None = None
    format: str = ""
    status: str = ""
    episodes: int | None = None
    poster: PosterSet = field(default_factory=PosterSet)

    @property
    def thumb_url(self) -> str | None:
        return self.poster.small.webp or self.poster.small.jpg

    @property
    def poster_url(self) -> str | None:
        return self.poster.big.webp or self.poster.big.jpg


@dataclass(frozen=True)
class SearchResult:
    total: int | None = None
    page: int | None = None
    pages: int | None = None
    items: list[AnimeSummary] = field(default_factory=list)


@dataclass(frozen=True)
class AnimeUpdate:
    anime: AnimeSummary = field(default_factory=AnimeSummary)
    teams: list[Team] = field(default_factory=list)
    team_name: str = ""
    is_collab: bool = False
    kind: str = ""
    provider: str = ""
    episode: int | float | str | None = None
    published_at: str = ""


@dataclass(frozen=True)
class UpdatesResult:
    total: int | None = None
    items: list[AnimeUpdate] = field(default_factory=list)


@dataclass(frozen=True)
class AiringEntry:
    anime: AnimeSummary = field(default_factory=AnimeSummary)
    episode: int | float | str | None = None
    airing: str = ""
    finished: bool = False


@dataclass(frozen=True)
class ScheduleResult:
    days: dict[str, list[AiringEntry]] = field(default_factory=dict)


@dataclass(frozen=True)
class AnimeRelation:
    kind: str = ""
    anime: AnimeSummary = field(default_factory=AnimeSummary)


@dataclass(frozen=True)
class AnimeDetail:
    ref: str = ""
    ids: dict[str, Any] = field(default_factory=dict)
    titles: Titles = field(default_factory=Titles)
    description: str | None = None
    format: str = ""
    status: str = ""
    start_date: str = ""
    source: str = ""
    studio: str | None = None
    country: str = ""
    age_rating: str = ""
    episodes: EpisodeStats = field(default_factory=EpisodeStats)
    scores: Scores = field(default_factory=Scores)
    genres: list[GenreTag] = field(default_factory=list)
    tags: list[GenreTag] = field(default_factory=list)
    poster: PosterSet = field(default_factory=PosterSet)
    ua_posters: list[UaPoster] = field(default_factory=list)
    relations: list[AnimeRelation] = field(default_factory=list)
    similar: list[AnimeSummary] = field(default_factory=list)

    @property
    def cover_url(self) -> str | None:
        for images in (*(poster.images for poster in self.ua_posters), self.poster):
            for size in (images.big, images.medium):
                if size.webp or size.jpg:
                    return size.webp or size.jpg
        return None

    @property
    def external_links(self) -> list[tuple[str, str]]:
        links: list[tuple[str, str]] = []
        if self.ids.get("mal"):
            links.append(("MyAnimeList", f"https://myanimelist.net/anime/{self.ids['mal']}"))
        if self.ids.get("al"):
            links.append(("AniList", f"https://anilist.co/anime/{self.ids['al']}"))
        if self.ids.get("hikka"):
            links.append(("Shikimori", f"https://shikimori.one/animes/{self.ids['hikka']}"))
        if self.ids.get("imdb"):
            links.append(("IMDb", f"https://www.imdb.com/title/{self.ids['imdb']}/"))
        return links


@dataclass(frozen=True)
class PlayerResult:
    licensed: bool = False
    releases: list[Release] = field(default_factory=list)
