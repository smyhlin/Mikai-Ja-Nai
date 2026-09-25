from __future__ import annotations

import os
import threading
import time
from pathlib import Path
from typing import Any

import requests

from mikai_backend.errors import MikaiApiError
from mikai_backend.models import (
    AnimeDetail,
    AnimeRelation,
    AnimeSummary,
    AnimeUpdate,
    AiringEntry,
    Episode,
    EpisodeStats,
    GenreTag,
    ImageVariant,
    PlayerResult,
    PosterSet,
    Release,
    Scores,
    SearchResult,
    ScheduleResult,
    Source,
    Team,
    Titles,
    UpdatesResult,
    UaPoster,
)

BASE_URL = "https://api.mikai.me/public/v1"


def _load_api_key() -> str | None:
    env_path = Path(__file__).resolve().parent.parent / ".env"
    if not env_path.is_file():
        return None
    for raw_line in env_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        separator = "=" if "=" in line else ":"
        name, _, value = line.partition(separator)
        if name.strip().lower() in {"x-api-key", "api_key", "apikey"}:
            return value.strip() or None
    return None


class MikaiClient:
    def __init__(self, api_key: str | None = None) -> None:
        self.api_key = api_key or os.environ.get("MIKAI_API_KEY") or _load_api_key()
        self._session = requests.Session()
        self._rate_lock = threading.Condition()
        self._rate_limit: int | None = None
        self._rate_remaining: int | None = None
        self._rate_reset_at = 0.0
        self._retry_at = 0.0
        self._rate_in_flight = 0
        self._rate_waiters = 0
        self._rate_generation = 0
        if self.api_key:
            self._session.headers["X-API-Key"] = self.api_key

    def search(self, query: str, page: int = 1, limit: int = 20) -> SearchResult:
        params: dict[str, Any] = {"page": page, "limit": limit}
        if query.strip():
            params["search"] = query.strip()
        payload = self._get("/anime", params)
        return SearchResult(
            total=payload.get("total"),
            page=payload.get("page") or page,
            pages=payload.get("pages"),
            items=[_parse_summary(entry) for entry in payload.get("result") or []],
        )

    def browse_ongoing(self, page: int = 1, limit: int = 24) -> SearchResult:
        """Return current shows ordered by the most recently added episode."""
        return self._browse_catalog(
            {"statuses": "ongoing", "sort": "updated", "order": "desc"}, page, limit
        )

    def browse_popular(self, page: int = 1, limit: int = 18) -> SearchResult:
        """Return the most viewed catalog titles for the home recommendations."""
        return self._browse_catalog({"sort": "views", "order": "desc"}, page, limit)

    def get_updates(self, page: int = 1, limit: int = 12) -> UpdatesResult:
        payload = self._get("/updates", {"page": page, "limit": limit})
        return UpdatesResult(
            total=payload.get("total"),
            items=[_parse_update(entry) for entry in payload.get("result") or []],
        )

    def get_schedule(self) -> ScheduleResult:
        payload = self._get("/schedule").get("result") or {}
        days = {
            day: [
                AiringEntry(
                    anime=_parse_summary(entry.get("anime") or {}),
                    episode=entry.get("episode"),
                    airing=entry.get("airing") or "",
                    finished=bool(entry.get("finished")),
                )
                for entry in payload.get(day) or []
            ]
            for day in (
                "monday",
                "tuesday",
                "wednesday",
                "thursday",
                "friday",
                "saturday",
                "sunday",
            )
        }
        return ScheduleResult(days=days)

    def _browse_catalog(self, filters: dict[str, Any], page: int, limit: int) -> SearchResult:
        params = {**filters, "page": page, "limit": limit}
        payload = self._get("/anime", params)
        return SearchResult(
            total=payload.get("total"),
            page=payload.get("page") or page,
            pages=payload.get("pages"),
            items=[_parse_summary(entry) for entry in payload.get("result") or []],
        )

    def get_anime(self, ref: str) -> AnimeDetail:
        return _parse_detail(self._get(f"/anime/{ref}")["result"])

    def get_player(self, ref: str) -> PlayerResult:
        return _parse_player(self._get(f"/anime/{ref}/player")["result"])

    def fetch_bytes(self, url: str) -> bytes:
        response = self._session.get(url, timeout=20)
        response.raise_for_status()
        return response.content

    def get_api_key(self) -> str | None:
        return self.api_key

    def set_api_key(self, api_key: str | None) -> None:
        cleaned = api_key.strip() if api_key and api_key.strip() else None
        self.api_key = cleaned
        if self.api_key:
            self._session.headers["X-API-Key"] = self.api_key
        else:
            self._session.headers.pop("X-API-Key", None)
        env_path = Path(__file__).resolve().parent.parent / ".env"
        if self.api_key:
            env_path.write_text(f"X-API-Key: {self.api_key}\n", encoding="utf-8")
        elif env_path.is_file():
            env_path.write_text("", encoding="utf-8")
        with self._rate_lock:
            self._rate_generation += 1
            self._rate_limit = None
            self._rate_remaining = None
            self._rate_reset_at = 0.0
            self._retry_at = 0.0
            self._rate_lock.notify_all()

    def validate_api_key(self, api_key: str | None) -> dict[str, Any]:
        cleaned = api_key.strip() if api_key and api_key.strip() else None
        headers = {"X-API-Key": cleaned} if cleaned else {}
        try:
            if cleaned == self.api_key:
                resp = self._api_get(f"{BASE_URL}/me", headers=headers, timeout=10)
            else:
                # A candidate key, including anonymous access, has its own
                # quota. Keep its response out of the saved key's counter.
                resp = requests.get(f"{BASE_URL}/me", headers=headers, timeout=10)
            data = resp.json()
            if data.get("ok"):
                res = data.get("result") or {}
                return {
                    "valid": True,
                    "authenticated": bool(res.get("authenticated")),
                    "rate_limit": res.get("rateLimitPerMin", 60),
                    "name": res.get("name", ""),
                }
            err = data.get("error") or {}
            return {"valid": False, "error": err.get("message", "invalid key")}
        except (requests.RequestException, ValueError, AttributeError, TypeError) as exc:
            return {"valid": False, "error": str(exc)}

    def _get(self, path: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        try:
            response = self._api_get(BASE_URL + path, params=params, timeout=15)
        except requests.RequestException as exc:
            raise MikaiApiError("network_error", str(exc), 0) from exc

        try:
            payload = response.json()
        except ValueError:
            raise MikaiApiError(
                "http_error", response.reason or "non-JSON response", response.status_code
            ) from None

        if not payload.get("ok"):
            error = payload.get("error") or {}
            raise MikaiApiError(
                error.get("code", "unknown"),
                error.get("message", response.reason or "request failed"),
                response.status_code,
            )
        return payload

    def rate_limit_state(self) -> dict[str, int | bool | None]:
        """Return the latest server quota snapshot and local queue size."""
        with self._rate_lock:
            now = time.monotonic()
            if self._rate_reset_at and now >= self._rate_reset_at:
                self._rate_remaining = None
                self._rate_reset_at = 0.0
            limit = self._rate_limit or (300 if self.api_key else 60)
            remaining = self._rate_remaining
            return {
                "limit": limit,
                "remaining": remaining,
                "used": max(0, limit - remaining) if remaining is not None else None,
                "reset_in": max(0, int(self._rate_reset_at - now + 0.999))
                if self._rate_reset_at
                else 0,
                "queued": self._rate_waiters,
                "in_flight": self._rate_in_flight,
            }

    def _api_get(self, url: str, **kwargs: Any) -> requests.Response:
        """Gate concurrent API calls on the server's rate-limit headers."""
        while True:
            generation = self._acquire_api_slot()
            try:
                response = self._session.get(url, **kwargs)
            except requests.RequestException:
                self._finish_api_call(None, generation)
                raise
            self._finish_api_call(response.headers, generation, response.status_code)
            if response.status_code != 429:
                return response

    def _acquire_api_slot(self) -> int:
        waiting = False
        with self._rate_lock:
            while True:
                now = time.monotonic()
                if self._rate_reset_at and now >= self._rate_reset_at:
                    self._rate_remaining = None
                    self._rate_reset_at = 0.0
                reset_wait = max(0.0, self._rate_reset_at - now)
                retry_wait = max(0.0, self._retry_at - now)
                quota_wait = (
                    reset_wait
                    if self._rate_remaining is not None
                    and self._rate_remaining <= self._rate_in_flight
                    else 0.0
                )
                delay = max(retry_wait, quota_wait)
                if delay <= 0:
                    if waiting:
                        self._rate_waiters -= 1
                    self._rate_in_flight += 1
                    return self._rate_generation
                if not waiting:
                    self._rate_waiters += 1
                    waiting = True
                self._rate_lock.wait(delay + 0.05)

    def _finish_api_call(self, headers: Any, generation: int, status_code: int = 0) -> None:
        with self._rate_lock:
            self._rate_in_flight = max(0, self._rate_in_flight - 1)
            if generation != self._rate_generation:
                self._rate_lock.notify_all()
                return
            if headers is not None:
                limit = _header_int(headers.get("X-RateLimit-Limit"))
                remaining = _header_int(headers.get("X-RateLimit-Remaining"))
                reset = _header_int(headers.get("X-RateLimit-Reset"))
                if limit is not None:
                    self._rate_limit = limit
                if remaining is not None:
                    # Headers from concurrent requests can arrive out of order.
                    # Keep the conservative value until the advertised reset.
                    if self._rate_reset_at and time.monotonic() < self._rate_reset_at:
                        if self._rate_remaining is None:
                            self._rate_remaining = remaining
                        else:
                            self._rate_remaining = min(self._rate_remaining, remaining)
                    else:
                        self._rate_remaining = remaining
                if reset is not None:
                    self._rate_reset_at = time.monotonic() + reset
                if status_code == 429:
                    retry_after = _header_int(headers.get("Retry-After"), 1)
                    self._retry_at = max(self._retry_at, time.monotonic() + retry_after)
            self._rate_lock.notify_all()


def _header_int(value: Any, default: int | None = None) -> int | None:
    try:
        return max(0, int(value))
    except TypeError, ValueError:
        return default


def _parse_titles(data: dict[str, Any]) -> Titles:
    return Titles(ua=data.get("ua"), english=data.get("english"), original=data.get("original"))


def _parse_variant(data: dict[str, Any] | None) -> ImageVariant:
    data = data or {}
    return ImageVariant(webp=data.get("webp"), jpg=data.get("jpg"))


def _parse_posters(data: dict[str, Any] | None) -> PosterSet:
    data = data or {}
    return PosterSet(
        big=_parse_variant(data.get("big")),
        medium=_parse_variant(data.get("medium")),
        small=_parse_variant(data.get("small")),
    )


def _parse_summary(entry: dict[str, Any]) -> AnimeSummary:
    ids = entry.get("ids") or {}
    ref = ids.get("mikai")
    if not ref:
        for key in ("mal", "al", "hikka", "slug"):
            if ids.get(key):
                ref = f"{key}:{ids[key]}"
                break
    return AnimeSummary(
        ref=str(ref or ""),
        titles=_parse_titles(entry.get("titles") or {}),
        year=entry.get("year"),
        format=entry.get("format") or "",
        poster=_parse_posters((entry.get("images") or {}).get("poster")),
    )


def _parse_update(entry: dict[str, Any]) -> AnimeUpdate:
    return AnimeUpdate(
        anime=_parse_summary(entry.get("anime") or {}),
        teams=[
            Team(
                id=team.get("id", 0),
                slug=team.get("slug") or "",
                name=team.get("name") or "",
            )
            for team in entry.get("teams") or []
        ],
        team_name=entry.get("teamName") or "",
        is_collab=bool(entry.get("isCollab")),
        kind=entry.get("kind") or "",
        provider=entry.get("provider") or "",
        episode=entry.get("episode"),
        published_at=entry.get("publishedAt") or "",
    )


def _parse_detail(data: dict[str, Any]) -> AnimeDetail:
    ids = data.get("ids") or {}
    episodes = data.get("episodes") or {}
    scores = data.get("scores") or {}
    return AnimeDetail(
        ref=str(ids.get("mikai", "")),
        ids=ids,
        titles=_parse_titles(data.get("titles") or {}),
        description=data.get("description"),
        format=data.get("format") or "",
        status=data.get("status") or "",
        start_date=data.get("startDate") or "",
        source=data.get("source") or "",
        studio=data.get("studio"),
        country=data.get("country") or "",
        age_rating=data.get("ageRating") or "",
        episodes=EpisodeStats(
            total=episodes.get("total"),
            aired=episodes.get("aired") or 0,
            localized=episodes.get("localized") or 0,
            duration_min=episodes.get("durationMin"),
        ),
        scores=Scores(
            mikai=scores.get("mikai"),
            mikai_count=scores.get("mikaiCount") or 0,
            mal=scores.get("mal"),
            mal_count=scores.get("malCount") or 0,
        ),
        genres=[
            GenreTag(name=genre.get("name") or "", ua=genre.get("ua"))
            for genre in data.get("genres") or []
        ],
        tags=[
            GenreTag(name=tag.get("name") or "", ua=tag.get("ua")) for tag in data.get("tags") or []
        ],
        poster=_parse_posters((data.get("images") or {}).get("poster")),
        ua_posters=[
            UaPoster(images=_parse_posters(poster.get("images")))
            for poster in data.get("uaPosters") or []
        ],
        relations=[
            AnimeRelation(
                kind=relation.get("kind") or "",
                anime=_parse_summary(relation.get("anime") or {}),
            )
            for relation in data.get("relations") or []
        ],
        similar=[_parse_summary(entry) for entry in data.get("similar") or []],
    )


def _parse_player(data: dict[str, Any]) -> PlayerResult:
    releases = []
    for raw_release in data.get("releases") or []:
        episodes = []
        for raw_episode in raw_release.get("episodes") or []:
            episodes.append(
                Episode(
                    number=raw_episode.get("number"),
                    label=raw_episode.get("label"),
                    sources=[
                        Source(
                            provider=source.get("provider") or "?",
                            embed_url=source.get("embedUrl") or "",
                        )
                        for source in raw_episode.get("sources") or []
                    ],
                )
            )
        releases.append(
            Release(
                id=raw_release.get("id") or "",
                kind=raw_release.get("kind") or "",
                is_collab=bool(raw_release.get("isCollab")),
                teams=[
                    Team(
                        id=team.get("id", 0),
                        slug=team.get("slug") or "",
                        name=team.get("name") or "",
                    )
                    for team in raw_release.get("teams") or []
                ],
                episodes_count=raw_release.get("episodesCount"),
                episodes=episodes,
            )
        )
    return PlayerResult(licensed=bool(data.get("licensed")), releases=releases)
