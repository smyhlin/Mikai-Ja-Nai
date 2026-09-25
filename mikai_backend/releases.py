from __future__ import annotations

from mikai_backend.models import Release

KIND_ORDER = {"voice": 0, "sub": 1}


def playable_episode_count(release: Release) -> int:
    if not release.episodes:
        return release.episodes_count or 0
    return sum(
        1 for episode in release.episodes if any(source.embed_url for source in episode.sources)
    )


def sort_releases(releases: list[Release]) -> list[Release]:
    return sorted(
        releases,
        key=lambda release: (
            KIND_ORDER.get(release.kind, 2),
            -playable_episode_count(release),
            ", ".join(team.name for team in release.teams).lower(),
        ),
    )
