"""Which home-page feed blocks are shown.

Every enabled feed costs API calls on each home load, so a user on a metered or slow
connection can switch the ones they do not read off. A disabled block is never
requested and never rendered.
"""

from __future__ import annotations

from typing import Any

ALL_FEEDS = ("ongoing", "popular", "updates", "schedule")

FEED_LABELS = {
    "ongoing": "Зараз виходять",
    "popular": "Популярні на Mikai",
    "updates": "Останні оновлення",
    "schedule": "Розклад виходу",
}

SETTINGS_KEY = "home/feeds"


def enabled(settings: Any) -> set[str]:
    """A store that never saw the key means every feed is on."""
    raw = settings.value(SETTINGS_KEY)
    if raw is None:
        return set(ALL_FEEDS)
    return {feed for feed in str(raw).split(",") if feed in ALL_FEEDS}


def save(settings: Any, feeds: set[str]) -> None:
    settings.setValue(SETTINGS_KEY, ",".join(feed for feed in ALL_FEEDS if feed in feeds))
