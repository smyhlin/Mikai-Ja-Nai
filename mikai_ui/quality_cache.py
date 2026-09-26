"""Cross-session store for extracted HLS quality URLs.

Stream URLs are signed and expire, so entries live for QUALITY_CACHE_TTL_MS only;
`TitlePage._recover_stream` re-extracts a stream that fails to play, and an entry that
aged out simply extracts again.
"""

from __future__ import annotations

import json
import time
from typing import Any

from mikai_ui.theme.metrics import QUALITY_CACHE_MAX_ENTRIES, QUALITY_CACHE_TTL_MS

SETTINGS_KEY = "quality/urls"


def load(settings: Any) -> tuple[dict[str, dict[int, str]], dict[str, float]]:
    """Return the cached streams and their fetch times, dropping anything past the TTL."""
    raw = settings.value(SETTINGS_KEY, "")
    if not isinstance(raw, str) or not raw:
        return {}, {}
    try:
        entries = json.loads(raw)["entries"]
    except ValueError, TypeError, KeyError:
        return {}, {}
    if not isinstance(entries, dict):
        return {}, {}
    cutoff = time.time() - QUALITY_CACHE_TTL_MS / 1000
    cache: dict[str, dict[int, str]] = {}
    stamps: dict[str, float] = {}
    for embed, entry in entries.items():
        try:
            fetched_at = float(entry["at"])
            heights = {int(height): url for height, url in entry["heights"].items() if url}
        except AttributeError, KeyError, TypeError, ValueError:
            continue
        if fetched_at >= cutoff and heights:
            cache[embed] = heights
            stamps[embed] = fetched_at
    return cache, stamps


def save(
    settings: Any,
    cache: dict[str, dict[int, str]],
    fetched_at: dict[str, float],
) -> None:
    cutoff = time.time() - QUALITY_CACHE_TTL_MS / 1000
    entries = {
        embed: {
            "at": fetched_at[embed],
            "heights": {str(height): url for height, url in heights.items()},
        }
        for embed, heights in cache.items()
        if heights and fetched_at.get(embed, 0.0) >= cutoff
    }
    if len(entries) > QUALITY_CACHE_MAX_ENTRIES:
        newest = sorted(entries.items(), key=lambda item: item[1]["at"], reverse=True)
        entries = dict(newest[:QUALITY_CACHE_MAX_ENTRIES])
    settings.setValue(SETTINGS_KEY, json.dumps({"entries": entries}, ensure_ascii=False))
