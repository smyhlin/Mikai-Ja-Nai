# Changelog

Published changes are recorded with their downloads on the [GitHub releases page](https://github.com/smyhlin/Mikai-Ja-Nai/releases). Artifact checksums live with each release, not in the documentation.

## Unreleased

- Correct disabled-state detection for retained Linux desktop entries, and safely refresh or remove stale owned entries.
- Add opt-in presentation profiling and a cross-process focus/composition probe.
- Retire Runner work and dispatch references without destroying thread pools on their own workers.
- Preserve PQ/BT.2020 and BT.709 frame metadata in generated acceptance media.
- Show the installed component inventory as soon as Settings opens instead of waiting for a manual Refresh.

- Offer MoonAnime opening, ending and recap skips, with accent-tinted timeline ranges.
- Show frame previews and segment labels on timeline hover.
- Keep identical hover requests in flight instead of restarting their decode; ignore late replies after the current request finishes or is refused.
- Initialize video-scaling captions on the first Settings display and retain native combo-box arrows.
- Synchronize player chrome with native fullscreen/windowed state changes.
- Resize and right-anchor PiP clock labels when their text changes so the remaining time is not clipped.
- Update advertised API quota estimates to 299 requests/minute without a key and 599 with an API key; server quota headers remain authoritative.
- Add a responsive Home/Filters/Teams sidebar with matching Open video, Shortcuts and Settings rows at the bottom and an icon rail at compact widths; keep search and RPM in a single toolbar.
- Let Home headings scroll with the feeds, reserve a scrollbar gutter and page carousels by the visible card count without diverting vertical scrolling.
- Prefer MoonAnime when that provider is present; an explicit choice still wins, and a title without it keeps its first provider.
- Keep a presented picture across source changes and ordinary chrome repaints instead of clearing to black. A chrome-only repaint no longer consumes an armed presentation slot.
- Keep cached controls pixel-stable when neighboring controls hide at fractional display scales. Remove the experimental shared chrome sheets and retain the 8 MiB per-list cache budget.
- Show "Preparing enhancement — playback paused" while an engine build holds playback, and "Finishing enhancement…" if closing that player takes more than a third of a second. The close wait is bounded at two seconds; if the native stop has not acknowledged by then, the close continues and the abandonment is logged.
- Add **Video scaling** after HDR playback: Fit, Fill, Stretch, 16:9, 4:3, original size, zoom, upscaler and downscaler. The first five modes are quick switches in the player settings flyout. PiP follows the display policy; Compare forces Fit while it owns the picture. Paused changes redraw after the asynchronous option writes complete. Windows framebuffer geometry and real Settings, live Fill/Fit, hover-preview and PiP screenshots are recorded; Linux pixel verification remains deferred.

## 0.1.0 — Player and usability update

The [update release](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0-update-20261101) covers the title-page redesign, player volume and seeking fixes, Compare exports, appearance settings, packaging and benchmark fixes, and AnimeJaNai diagnostics.

## 0.1.0 — First public build

The [first release](https://github.com/smyhlin/Mikai-Ja-Nai/releases/tag/v0.1.0) introduced the Qt desktop client, libmpv player, optional processing, Compare workstation and Windows/Linux packages.

For implementation details, see the [architecture](docs/architecture.md), [player guide](docs/player-guide.md), [processing guide](docs/processing-guide.md) and [AnimeJaNai troubleshooting](docs/animejanai.md). Historical verification and open limitations are recorded in [release status](docs/release-status.md) and [redistribution evidence](docs/redistribution.md).
