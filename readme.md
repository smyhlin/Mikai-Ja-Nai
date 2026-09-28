# Mikai Wrapper

A PySide6 desktop client for the [Mikai public API](https://api.mikai.me/public/docs).

## Run

```powershell
uv sync
uv run mikai-wrapper
```

If the Windows `uv.exe` launcher reports a trampoline error, use `python -m uv run mikai-wrapper`.

## What it does

- **Home** — carousels for titles you recently opened, titles airing now, and Mikai's current popular picks, plus the latest episode updates and the weekly airing schedule. Every card shows the poster, the airing status and the aired/planned episode count, and a search result row reads the same way (`Бліч · 2004 · TV · Завершено · 366 серій`) straight from the list response, with no extra request; right-click a card to open it, copy its mikai.me link or raw id, or open the site page in a browser.
- **Title** — description, cover, scores, genres and related titles, then one tab per release (voice or subtitles, grouped by dubbing team). Each episode offers the qualities the provider actually has; a left click copies that stream URL and opens the player, the right-click menu adds copy-URL-only, re-extract, and hand-off to an external player. The cover opens full size with pan and zoom.
- **Player** — a full-screen player: the picture fills the screen edge to edge, the controls float over it and fade away after a couple of seconds of stillness, returning on the next mouse move. Click the picture to pause, step through episodes from the drawer, and switch the dub/sub track or the stream quality without leaving the player. A picture-in-picture float drops the video into a small always-on-top window with its own overlaid controls — click to pause, hold and drag to move it.
- **Settings** — the API token with its live quota, plus a checklist of the home blocks to load. A block you switch off is never requested, which is the cheapest way to cut API spend on a slow or metered connection.

Qualities for the whole visible episode page are extracted at once, and the resulting stream URLs are cached for half an hour, so reopening a title repaints instead of re-hitting every provider. The links are signed and do expire; if a stream fails to play the player re-extracts it once on its own before reporting the error.

## API limits

The **RPM** indicator beside Settings shows the server-reported requests used in the current minute and the limit that applies. Calls wait for the server's reset window instead of failing when the allowance is spent. An optional API key raises the standard limit from 60 to 300 requests per minute and is stored locally in `.env`, which is never committed.

## Layout

`mikai_backend/` is the Qt-free domain — client and rate limiter, models, HLS extraction, libmpv discovery — and `mikai_ui/` is the PySide6 frontend, whose player is the vendored media-lightbox shell in `mikai_ui/media_shell.py` with the mikai-specific panels composed around it in `mikai_ui/shell_player.py`. `docs.md` is the authoritative API contract and `AGENTS.md` the map for contributors: architecture, conventions and the testing setup. Run the checks with `uv run pytest tests/ -v`.
